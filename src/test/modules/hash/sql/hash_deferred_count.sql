-- Test the deferred tuple count of hash index inserts.
--
-- An insert that finds the metapage locked leaves its tuple pending instead
-- of waiting for the lock.  Once a backend has done 1024 inserts into the
-- index, a failed try also makes it start batching: it only tries the lock
-- on every 32nd insert.  It waits for the lock once 256 tuples would be
-- pending.  The hash-insert-defer-count injection point makes every try act
-- as if the lock was taken.
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
SELECT injection_points_set_local();

CREATE TABLE hash_count_test (v int4) WITH (autovacuum_enabled = false);
CREATE INDEX hash_count_index ON hash_count_test USING hash (v);

-- Without contention, the count is exact
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 10) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- Deferred inserts leave the count alone
SELECT injection_points_attach('hash-insert-defer-count', 'notice');
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 20) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- Before 1024 inserts, every insert tries the lock; the next one that gets
-- it counts what is pending
SELECT injection_points_detach('hash-insert-defer-count');
INSERT INTO hash_count_test VALUES (1);
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- After 1024 inserts, a failed try starts batching
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 1000) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));
SELECT injection_points_attach('hash-insert-defer-count', 'notice');
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 20) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- The insert that would leave 256 pending waits for the lock and counts
-- them all, leaving 64 of these 300 pending
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 300) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- With the lock free again, the backend still only tries it on every 32nd
-- insert: 31 more leave 95 pending, and the next one counts all 96
SELECT injection_points_detach('hash-insert-defer-count');
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 31) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));
INSERT INTO hash_count_test VALUES (1);
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));
SELECT count(*) FROM hash_count_test;

-- Each index has its own pending count: a new index counts every insert
CREATE INDEX hash_count_index2 ON hash_count_test USING hash (v);
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 5) g;
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index', 0));
SELECT ntuples FROM hash_metapage_info(get_raw_page('hash_count_index2', 0));

-- A deferred count cannot stop the index from splitting, because at most
-- 255 tuples stay pending.  Build a 2-bucket index, then insert with every
-- try of the lock failing.
DROP INDEX hash_count_index, hash_count_index2;
TRUNCATE hash_count_test;
INSERT INTO hash_count_test SELECT 1 FROM generate_series(1, 10);
ANALYZE hash_count_test;
CREATE INDEX hash_count_index ON hash_count_test USING hash (v);
SELECT maxbucket FROM hash_metapage_info(get_raw_page('hash_count_index', 0));
SELECT injection_points_attach('hash-insert-defer-count', 'notice');
INSERT INTO hash_count_test SELECT g FROM generate_series(1, 10000) g;
SELECT injection_points_detach('hash-insert-defer-count');
SELECT maxbucket > 16 AS split,
       ntuples >= 10010 - 255 AND ntuples <= 10010 AS count_ok
  FROM hash_metapage_info(get_raw_page('hash_count_index', 0));

-- Index scans see the deferred tuples
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM hash_count_test WHERE v = 1;
SELECT count(*) FROM hash_count_test WHERE v = 9999;
RESET enable_seqscan;
RESET enable_bitmapscan;

DROP TABLE hash_count_test;
DROP EXTENSION pageinspect;
DROP EXTENSION injection_points;
