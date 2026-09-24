# As in index-only-scan.spec, each transaction deletes the row identified by
# an index-only scan of the other table.  The UPDATE variant sets present to
# false so no new index entry can reveal the conflict.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE ios_a (id int PRIMARY KEY) WITH (autovacuum_enabled = false);
	CREATE TABLE ios_b (id int, present bool DEFAULT true)
		WITH (autovacuum_enabled = false);
	CREATE INDEX ios_b_idx ON ios_b (id) WHERE present;
	INSERT INTO ios_a VALUES (1);
	INSERT INTO ios_b VALUES (1);
}

# Wait until VACUUM can mark the new rows all-visible.
setup
{
	DO $$
	DECLARE
		barrier xid8 := pg_current_xact_id();
	BEGIN
		LOOP
			ROLLBACK;  -- release our own snapshot
			EXIT WHEN removable_cutoff('pg_database') >= barrier;
			PERFORM pg_sleep(0.1);
		END LOOP;
	END $$;
}
setup { VACUUM (FREEZE, ANALYZE) ios_a, ios_b; }
teardown
{
	DROP TABLE ios_a, ios_b;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	BEGIN ISOLATION LEVEL SERIALIZABLE;
	-- Run the scan in the backend that owns the local injection point.
	SET LOCAL max_parallel_workers_per_gather = 0;
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SELECT FROM injection_points_set_local();
}
step pause_reader
{
	SELECT FROM injection_points_attach('index-only-scan-before-predicate-lock', 'wait');
}
step delete1
{
	DELETE FROM ios_a
	WHERE id = (SELECT min(id) FROM ios_b WHERE present)
	RETURNING id;
}
step commit1 { COMMIT; }

step wake_delete
{
	SELECT FROM injection_points_detach('heap-delete-before-write');
	SELECT FROM injection_points_wakeup('heap-delete-before-write');
}
step wake_update
{
	SELECT FROM injection_points_detach('heap-update-before-write');
	SELECT FROM injection_points_wakeup('heap-update-before-write');
}
# Finish reporting the wakeup before the other session commits.
step done1 { }

session s2
setup
{
	BEGIN ISOLATION LEVEL SERIALIZABLE;
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SELECT FROM injection_points_set_local();
}
step pause_delete
{
	SELECT FROM injection_points_attach('heap-delete-before-write', 'wait');
}
step pause_update
{
	SELECT FROM injection_points_attach('heap-update-before-write', 'wait');
}
step delete2
{
	DELETE FROM ios_b
	WHERE id = (SELECT min(id) FROM ios_a)
	RETURNING id;
}
step update2
{
	UPDATE ios_b SET present = false
	WHERE id = (SELECT min(id) FROM ios_a)
	RETURNING id;
}
step commit2 { COMMIT; }

step wake_reader
{
	SELECT FROM injection_points_detach('index-only-scan-before-predicate-lock');
	SELECT FROM injection_points_wakeup('index-only-scan-before-predicate-lock');
}
step done2 { }

# s1 reads b after s2's first conflict check.  After clearing the VM bit,
# s2 must check again, find s1's predicate lock, and abort.
permutation pause_delete delete2 delete1 commit1 wake_delete(delete2) done1 commit2
permutation pause_update update2 delete1 commit1 wake_update(update2) done1 commit2

# s2 changes b after s1's first VM check but before s1's predicate lock.
# Rechecking the VM must make s1 fetch the heap tuple and detect s2's write,
# so that s1 cannot also commit its delete.
permutation pause_reader delete1 delete2 commit2 wake_reader(delete1) done2 commit1
permutation pause_reader delete1 update2 commit2 wake_reader(delete1) done2 commit1
