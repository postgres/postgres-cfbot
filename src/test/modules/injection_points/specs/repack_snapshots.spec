# REPACK (CONCURRENTLY) - use one snapshot per block range.
setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE repack_test(i int PRIMARY KEY, j text);
	CREATE INDEX ON repack_test(i DESC);
	CREATE TABLE relfilenodes(node oid);

	CREATE TABLE data_s1(i int, j text);
	CREATE TABLE data_s2(i int, j text);

	-- Keep inserting tuples into repack_test until several tuples need to
        -- be inserted into block number last_block.
	CREATE FUNCTION load(last_block int)
	RETURNS void
	LANGUAGE 'plpgsql'
	AS $$
	    DECLARE
		cnt	int;
	    BEGIN
		INSERT INTO repack_test VALUES (1, gen_external());

		LOOP
		    WITH max(m) AS (SELECT max(i) FROM repack_test)
		    INSERT INTO repack_test(i, j)
		    SELECT m + x, gen_external()
		    FROM generate_series(1, 100) s(x), max;

		    SELECT count(*)
		    FROM repack_test WHERE tid_block(ctid) = last_block
		    INTO cnt;

		    IF cnt >= 10 THEN
			EXIT;
		    END IF;
		END LOOP;
	    END;
	$$;

	-- Generate a string of random characters that is not likely to be
	-- compressed, but is big enough to be stored externally.
	CREATE FUNCTION gen_external()
	RETURNS text
	LANGUAGE sql as $$
		SELECT string_agg(chr(65 + trunc(25 * random())::int), '')
		FROM generate_series(1, 2048) s(x);
	$$;

	-- Wait until the number of blocks involved in the scan by REPACK has
	-- reached _blocks.
	CREATE FUNCTION wait_for_blocks_scanned(_relid oid, _blocks int)
	RETURNS void
	LANGUAGE 'plpgsql'
	AS $$
		DECLARE
			cnt	int;
		BEGIN
			LOOP
				PERFORM pg_stat_clear_snapshot();

				SELECT heap_blks_scanned
				FROM pg_stat_progress_repack
				WHERE relid = _relid
				INTO cnt;

				IF cnt >= _blocks THEN
					EXIT;
				END IF;

				PERFORM pg_sleep(0.1);
			END LOOP;
		END;
	$$;
}

teardown
{
	DROP TABLE repack_test;
	DROP EXTENSION injection_points;

	DROP TABLE relfilenodes;
	DROP TABLE data_s1;
	DROP TABLE data_s2;

	DROP FUNCTION load(int);
	DROP FUNCTION gen_external();
	DROP FUNCTION wait_for_blocks_scanned(oid, int);
}

session s1
setup
{
	SET repack_snapshot_after = 1;

	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-new-range', 'wait');
}
# The most practical way to test the corner cases is to set range size to 1
# block. To initialize, insert new tuples until we have several tuples in the
# 2nd block.
step load
{
	SELECT load(1);
}
# Start the initial load and wait when the first range has been completed.
step repack
{
	REPACK (CONCURRENTLY) repack_test;
}
# The same, but with clustering.
step repack_pkey
{
	REPACK (CONCURRENTLY) repack_test USING INDEX repack_test_pkey;
}
# Clustering by other than the identity index.
step repack_other_index
{
	REPACK (CONCURRENTLY) repack_test USING INDEX repack_test_i_idx;
}
# Check the table from the perspective of s1.
step check1
{
	INSERT INTO data_s1(i, j)
	SELECT i, j FROM repack_test;

	SELECT count(*) > 0 FROM repack_test;

	SELECT count(*)
	FROM data_s1 d1 FULL JOIN data_s2 d2 USING (i, j)
	WHERE d1.i ISNULL OR d2.i ISNULL;
}
# Check the ordering where appropriate. We don't know the exact number of
# rows, so just check a sample. (The replayed concurrent changes are not
# ordered, but those shouldn't fit into the first 10 rows.)
step check1_order_asc
{
	SELECT i FROM repack_test LIMIT 10;
}
# Due to the special way of loading the data (see the load() function above)
# we don't know the maximum value. To make the test output deterministic,
# check for cases where the current row is not lower than the previous row.
step check1_order_desc
{
	WITH tmp(diff) as (
		SELECT i - lag(i, 1, 10000) OVER (ORDER BY ctid)
		FROM repack_test
		LIMIT 10)
	SELECT * FROM tmp WHERE diff > -1;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-new-range');
}

session s2
# Test processing of changes such that the new tuple is beyond the current
# range. Specifically for UPDATE, the old tuple should be in the current
# range. So when applying it, we have to convert it to DELETE.
step change_new_beyond
{
	INSERT INTO repack_test
	SELECT max(i) + 1, gen_external()
	FROM repack_test
	RETURNING tid_block(ctid);

	UPDATE repack_test SET j = gen_external()
	WHERE ctid=(SELECT min(ctid) FROM repack_test)
	RETURNING tid_block(OLD.ctid), tid_block(NEW.ctid);
}
# Test processing of changes such that the old tuple is beyond the current
# range. The UPDATE puts also the new tuple beyond the current range.
step change_old_beyond
{
	DELETE FROM repack_test
	WHERE ctid=(SELECT max(ctid) FROM repack_test)
	RETURNING tid_block(ctid);

	UPDATE repack_test SET j = gen_external()
	WHERE ctid=(SELECT max(ctid) FROM repack_test)
	RETURNING tid_block(OLD.ctid), tid_block(NEW.ctid);
}
# Arrange for UPDATE to put the new tuples into block 0. In particular, fill
# the block 1 and delete some tuples from block 1.
step load2
{
	SELECT load(2);

	DELETE FROM repack_test WHERE i < 100;
}
# Logically this belongs to the previous step, however VACUUM cannot run
# inside a transaction block.
step load2_vacuum
{
	VACUUM repack_test;
}
# This UPDATE should put the new tuple into block 0 (the current range). So
# when replaying it, we have to convert it to INSERT. That includes fetching
# the old tuple's TOAST from the TOAST table because the old tuple is not
# available during the replay.
step change_old_beyond2
{
	UPDATE repack_test SET j = gen_external()
	WHERE ctid=(SELECT max(ctid) FROM repack_test WHERE tid_block(ctid) = 1)
	RETURNING tid_block(OLD.ctid), tid_block(NEW.ctid);
}
# Check the table from the perspective of s4.
step check2
{
	INSERT INTO data_s2(i, j)
	SELECT i, j FROM repack_test;
}
step wakeup_new_range
{
	SELECT injection_points_wakeup('repack-concurrently-new-range');
}
# This step is used to make sure that REPACK is waiting on an injection point
# before finalizing the 2nd block. However, the coding is such that the
# counter is incremented at the top of the loop (see
# heapam_relation_copy_for_cluster()), so we wait until it's at least 3.
step wait_new_range
{
	SELECT wait_for_blocks_scanned('repack_test'::regclass, 3);
}

# Test if snapshots are used correctly to scan block ranges.
permutation
	load
	repack
	change_new_beyond
	change_old_beyond
	check2
	wakeup_new_range
	check1

# Special attention is needed to update tuple in block 1 so that the new tuple
# appears in block 0. The preparation includes VACUUM, which in turn cannot
# proceed while REPACK is in progress. That's why we need a separate
# permutation. Note that two wake-ups are needed as we have two range
# boundaries now. However we need to wait in between to make sure that the
# second waiting started before we try to wake it up.
permutation
	load2
	load2_vacuum
	repack
	change_old_beyond2
	check2
	wakeup_new_range
	wait_new_range
	wakeup_new_range
	check1

# The first permutation with identity index as the clustering index.
permutation
	load
	repack_pkey
	change_new_beyond
	change_old_beyond
	check2
	wakeup_new_range
	check1
	check1_order_asc
# The first permutation with another clustering index.
permutation
	load
	repack_other_index
	change_new_beyond
	change_old_beyond
	check2
	wakeup_new_range
	check1
	check1_order_desc
