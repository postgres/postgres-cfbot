-- Test hash index scans of a bucket pair whose split is incomplete.
--
-- A scan whose target bucket is still being populated by a split must read
-- both buckets: the one being populated, skipping the tuples moved there,
-- and the one being split.
CREATE EXTENSION injection_points;
SELECT injection_points_set_local();

SELECT injection_points_attach('hash-finish-incomplete-split', 'notice');

-- Build an index on an analyzed 10-row table, so that it starts out with
-- just two buckets (metapage, two bucket pages, bitmap page).  Rows with
-- v = 1 hash to bucket 0, and to bucket 2 once bucket 0 splits.
CREATE TABLE hash_split_test (v int4) WITH (autovacuum_enabled = false);
INSERT INTO hash_split_test SELECT 1 FROM generate_series(1, 10);
ANALYZE hash_split_test;
CREATE INDEX hash_split_index ON hash_split_test USING hash (v);
SELECT pg_relation_size('hash_split_index') /
       current_setting('block_size')::int AS index_pages;

-- Error out during the first bucket split, leaving it incomplete
SELECT injection_points_attach('hash-split-before-relocation', 'error');
INSERT INTO hash_split_test SELECT g FROM generate_series(1000001, 1005000) g;
SELECT injection_points_detach('hash-split-before-relocation');

-- A backward index scan reads the bucket being split, then the one being
-- populated, which is still empty: it must end there
SET enable_seqscan = off;
SET enable_bitmapscan = off;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT v FROM hash_split_test WHERE v = 1;
MOVE FORWARD ALL IN c;
FETCH BACKWARD ALL FROM c;
COMMIT;

-- This row goes to the new bucket, which is still flagged as being populated
INSERT INTO hash_split_test VALUES (1);

-- Forward and backward index scans must find it there, plus the 10 older
-- rows that remain in bucket 0
EXPLAIN (COSTS OFF) SELECT count(*) FROM hash_split_test WHERE v = 1;
SELECT count(*) FROM hash_split_test WHERE v = 1;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT v FROM hash_split_test WHERE v = 1;
MOVE FORWARD ALL IN c;
FETCH BACKWARD ALL FROM c;
COMMIT;

-- An insert into bucket 0 (v = 5 hashes there) finishes the split, which
-- first relocates the 10 older rows to the new bucket.  Error out after
-- that, before the split is marked complete: the scan must skip the
-- relocated copies, and read the originals that remain in bucket 0.
SELECT injection_points_attach('hash-split-after-relocation', 'error');
INSERT INTO hash_split_test VALUES (5);
SELECT injection_points_detach('hash-split-after-relocation');
SELECT count(*) FROM hash_split_test WHERE v = 1;

-- Finish the split for real.  The scan must still find all 11 rows.
INSERT INTO hash_split_test VALUES (5);
SELECT count(*) FROM hash_split_test WHERE v = 1;

-- Bucket 2 now holds the 10 relocated rows, still marked as moved by split.
-- Three more splits (of buckets 1, 0 and 1 again) make bucket 2 the next
-- one to split, into bucket 6, which is where v = 1 hashes from then on.
-- The number of rows that it takes to trigger a split depends on the width
-- of a MAXALIGN'd index tuple, so rather than count rows, have each of the
-- three splits fail, then finish it with an insert into the bucket being
-- split (v = 6 and v = 11 hash to buckets 0 and 1).
DO $$
BEGIN
  FOR i IN 1..3 LOOP
    PERFORM injection_points_attach('hash-split-before-relocation', 'error');
    BEGIN
      INSERT INTO hash_split_test SELECT g FROM generate_series(2000001, 2005000) g;
    EXCEPTION WHEN OTHERS THEN
      RAISE NOTICE '%', SQLERRM;
    END;
    PERFORM injection_points_detach('hash-split-before-relocation');
    INSERT INTO hash_split_test VALUES (6), (11);
  END LOOP;
END $$;

-- Now error out before the split of bucket 2 relocates anything: a scan of
-- bucket 6 must read all 11 rows in bucket 2, marked or not.
SELECT injection_points_attach('hash-split-before-relocation', 'error');
INSERT INTO hash_split_test SELECT g FROM generate_series(3000001, 3005000) g;
SELECT injection_points_detach('hash-split-before-relocation');
SELECT count(*) FROM hash_split_test WHERE v = 1;

-- The heap agrees
SET enable_seqscan = on;
SET enable_indexscan = off;
SELECT count(*) FROM hash_split_test WHERE v = 1;

SELECT injection_points_detach('hash-finish-incomplete-split');
DROP TABLE hash_split_test;
DROP EXTENSION injection_points;
