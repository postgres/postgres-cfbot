CREATE EXTENSION IF NOT EXISTS test_locator_tableam;

--
-- An index can be built only on a table whose locator fits in an
-- ItemPointerData.  heap_wide is heap with a 16-byte "wide" locator, so every
-- path that builds an index on it must fail with the same error.
--

-- CREATE TABLE and CREATE INDEX
CREATE TABLE wide_t (a int, b text) USING heap_wide;
INSERT INTO wide_t SELECT g, 'x' || g FROM generate_series(1, 10) g;
CREATE INDEX wide_t_a ON wide_t (a);
CREATE INDEX CONCURRENTLY wide_t_a ON wide_t (a);
ALTER TABLE wide_t ADD PRIMARY KEY (a);
ALTER TABLE wide_t ADD UNIQUE (a);
CREATE INDEX wide_t_b ON wide_t USING hash (b);
CREATE TABLE wide_pk (a int PRIMARY KEY) USING heap_wide;
-- a table with no indexes works
SELECT count(*) FROM wide_t;

-- ALTER TABLE ... SET ACCESS METHOD rebuilds the table's indexes
CREATE TABLE narrow_t (a int, b text) USING heap;
CREATE INDEX narrow_t_a ON narrow_t (a);
INSERT INTO narrow_t SELECT g, 'x' || g FROM generate_series(1, 10) g;
ALTER TABLE narrow_t SET ACCESS METHOD heap_wide;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
 WHERE c.oid = 'narrow_t'::regclass;
-- without the index it works, and an index cannot then be added back
DROP INDEX narrow_t_a;
ALTER TABLE narrow_t SET ACCESS METHOD heap_wide;
CREATE INDEX narrow_t_a ON narrow_t (a);
-- VACUUM FULL and REINDEX TABLE have no index to rebuild
VACUUM FULL narrow_t;
REINDEX TABLE narrow_t;
-- and back to heap
ALTER TABLE narrow_t SET ACCESS METHOD heap;
CREATE INDEX narrow_t_a ON narrow_t (a);
REINDEX INDEX narrow_t_a;
REINDEX TABLE CONCURRENTLY narrow_t;

-- partitioned tables: the check applies to each partition's table AM
CREATE TABLE wide_p (a int, b text) PARTITION BY RANGE (a);
CREATE TABLE wide_p1 PARTITION OF wide_p FOR VALUES FROM (0) TO (100);
CREATE TABLE wide_p2 PARTITION OF wide_p FOR VALUES FROM (100) TO (200)
  USING heap_wide;
CREATE INDEX wide_p_a ON wide_p (a);
DROP TABLE wide_p2;
CREATE INDEX wide_p_a ON wide_p (a);
-- a new partition gets the parent's indexes
CREATE TABLE wide_p2 PARTITION OF wide_p FOR VALUES FROM (100) TO (200)
  USING heap_wide;
CREATE TABLE wide_p3 (a int, b text) USING heap_wide;
ALTER TABLE wide_p ATTACH PARTITION wide_p3 FOR VALUES FROM (200) TO (300);
-- a partitioned table has no locator of its own, but its partitions do
CREATE TABLE wide_q (a int PRIMARY KEY) PARTITION BY LIST (a) USING heap_wide;
CREATE TABLE wide_q1 PARTITION OF wide_q FOR VALUES IN (1);

-- An existing index is checked again whenever it is rebuilt, which matters if
-- the table AM's locator changes under it (here, as an extension upgrade
-- might, by repointing the AM's handler).
CREATE ACCESS METHOD heap_later TYPE TABLE HANDLER heap_tableam_handler;
CREATE TABLE later_t (a int) USING heap_later;
CREATE INDEX later_t_a ON later_t (a);
INSERT INTO later_t SELECT generate_series(1, 10);
UPDATE pg_am SET amhandler = (SELECT amhandler FROM pg_am
                               WHERE amname = 'heap_wide')
 WHERE amname = 'heap_later';
\c
REINDEX INDEX later_t_a;
REINDEX TABLE later_t;
REINDEX INDEX CONCURRENTLY later_t_a;
TRUNCATE later_t;
VACUUM FULL later_t;
UPDATE pg_am SET amhandler = 'heap_tableam_handler'::regproc
 WHERE amname = 'heap_later';
\c
REINDEX TABLE later_t;

DROP TABLE wide_t, narrow_t, wide_p, wide_p3, wide_q, later_t;
DROP ACCESS METHOD heap_later;
