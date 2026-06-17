--
-- GLOBAL TEMP
--
CREATE SCHEMA global_temp_tests;
GRANT USAGE ON SCHEMA global_temp_tests TO PUBLIC;
SET search_path = global_temp_tests;
CREATE ROLE regress_global_temp_user;
GRANT CREATE ON SCHEMA global_temp_tests TO regress_global_temp_user;
GRANT CREATE ON DATABASE regression TO regress_global_temp_user;
SET ROLE regress_global_temp_user;

-- Test table creation
CREATE GLOBAL TEMP TABLE pg_temp.tmp1 (a int); -- fail
CREATE GLOBAL TEMP TABLE tmp1 (a int PRIMARY KEY, b text, c serial);
CREATE SCHEMA global_temp_xxx CREATE GLOBAL TEMP TABLE tmp2 (a int);
CREATE SCHEMA global_temp_yyy;
CREATE GLOBAL TEMP TABLE global_temp_yyy.tmp3 (a int);

\d tmp1
\dt+ global_temp_*.tmp*

-- Information schema
SELECT table_catalog, table_schema, table_name, table_type
FROM information_schema.tables
WHERE table_name ~ 'tmp' AND table_schema ~ 'global_temp'
ORDER BY table_name;

-- Check pg_stat_activity
SELECT tempfrozenxid, tempminmxid -- not visible without privilege
  FROM pg_stat_activity
 WHERE pid = pg_backend_pid();

RESET ROLE;
GRANT pg_read_all_stats TO regress_global_temp_user;
SET ROLE regress_global_temp_user;

SELECT tempfrozenxid::text::bigint - (SELECT min(relfrozenxid::text::bigint)
                                        FROM pg_temp_class
                                       WHERE relfrozenxid::text != '0'),
       tempminmxid::text::bigint - (SELECT min(relminmxid::text::bigint)
                                      FROM pg_temp_class
                                     WHERE relminmxid::text != '0')
  FROM pg_stat_activity
 WHERE pid = pg_backend_pid();

DROP SCHEMA global_temp_xxx CASCADE;
DROP SCHEMA global_temp_yyy CASCADE;

-- Basic tests
INSERT INTO tmp1 VALUES (1, 'xxx');
SELECT * FROM tmp1;
\c
SET search_path = global_temp_tests;
SELECT oid::regclass FROM pg_temp_class ORDER BY 1;
SELECT * FROM tmp1;
SELECT oid::regclass FROM pg_temp_class ORDER BY 1;

-- Test pg_relation_filenode() matches global relfilenode
SELECT relfilenode = pg_relation_filenode('tmp1'::regclass) AS ok
  FROM pg_class WHERE oid = 'tmp1'::regclass;

-- Test index
INSERT INTO tmp1 VALUES (1, 'xxx');
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT * FROM tmp1 WHERE a = 1;
SELECT * FROM tmp1 WHERE a = 1;
RESET enable_seqscan;

-- Test concurrent index build -- CONCURRENTLY is ignored with temp tables
CREATE INDEX CONCURRENTLY tmp1_b_idx ON tmp1(b);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT * FROM tmp1 WHERE b = 'xxx';
SELECT * FROM tmp1 WHERE b = 'xxx';
RESET enable_seqscan;
REINDEX INDEX CONCURRENTLY tmp1_b_idx;
REINDEX TABLE CONCURRENTLY tmp1;

-- Test REINDEX -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1_b_idx' \gset
REINDEX INDEX tmp1_b_idx;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1_b_idx';
DROP INDEX CONCURRENTLY tmp1_b_idx;

-- REINDEX not allowed on pg_temp_class
REINDEX INDEX pg_temp_class_oid_index;
REINDEX TABLE pg_temp_class;

-- Test ON COMMIT DELETE ROWS
CREATE GLOBAL TEMP TABLE tmp2 (a int) ON COMMIT DELETE ROWS;
BEGIN;
INSERT INTO tmp2 VALUES (1);
SELECT * FROM tmp2;
COMMIT;
SELECT * FROM tmp2;

-- Repeat test in a new session
\c
SET search_path = global_temp_tests;
BEGIN;
INSERT INTO tmp2 VALUES (1);
SELECT * FROM tmp2;
COMMIT;
SELECT * FROM tmp2;
DROP TABLE tmp2;

-- ON COMMIT DROP not allowed
CREATE GLOBAL TEMP TABLE tmp2 (a int) ON COMMIT DROP; -- fail

-- Two-phase commit not allowed with global temp tables
BEGIN;
SELECT * FROM tmp1;
PREPARE TRANSACTION 'twophase'; -- fail

-- Test partitioned global temp table
CREATE GLOBAL TEMP TABLE tmp2 (a int) PARTITION BY LIST (a);
CREATE GLOBAL TEMP TABLE tmp2_p1 PARTITION OF tmp2 FOR VALUES IN (1);
CREATE GLOBAL TEMP TABLE tmp2_p2 (a int);
ALTER TABLE tmp2 ATTACH PARTITION tmp2_p2 FOR VALUES IN (2);

CREATE TEMP TABLE local_tmp PARTITION OF tmp2 FOR VALUES IN (3); -- fail
CREATE TEMP TABLE local_tmp (a int);
ALTER TABLE tmp2 ATTACH PARTITION local_tmp FOR VALUES IN (3); -- fail

CREATE TABLE perm PARTITION OF tmp2 FOR VALUES IN (3); -- fail
CREATE TABLE perm (a int);
ALTER TABLE tmp2 ATTACH PARTITION perm FOR VALUES IN (3); -- fail

INSERT INTO tmp2 VALUES (1), (2);
SELECT tableoid::regclass, * FROM tmp2 ORDER BY a;
\c
SET search_path = global_temp_tests;
SELECT tableoid::regclass, * FROM tmp2 ORDER BY a;
DROP TABLE tmp2, perm;

-- Test ALTER TABLE with rewrite
CREATE GLOBAL TEMP TABLE tmp2 (a int);
INSERT INTO tmp2 VALUES (1);
ALTER TABLE tmp2 ALTER COLUMN a SET DATA TYPE numeric;
SELECT a, pg_typeof(a) FROM tmp2;
DROP TABLE tmp2;

-- Test foreign keys
CREATE TABLE perm_pk_rel (a int PRIMARY KEY);
CREATE TEMP TABLE temp_pk_rel (a int PRIMARY KEY);
CREATE GLOBAL TEMP TABLE gtemp_pk_rel (a int PRIMARY KEY);
CREATE TABLE tmp2 (a int REFERENCES gtemp_pk_rel); -- fail
CREATE TEMP TABLE tmp2 (a int REFERENCES gtemp_pk_rel); -- fail
CREATE GLOBAL TEMP TABLE tmp2 (a int REFERENCES perm_pk_rel); -- fail
CREATE GLOBAL TEMP TABLE tmp2 (a int REFERENCES temp_pk_rel); -- fail
CREATE GLOBAL TEMP TABLE tmp2 (a int REFERENCES gtemp_pk_rel);
INSERT INTO gtemp_pk_rel VALUES (1);
INSERT INTO tmp2 VALUES (1);
INSERT INTO tmp2 VALUES (2); -- fail
DELETE FROM gtemp_pk_rel WHERE a = 1; -- fail
ALTER TABLE tmp2 DROP CONSTRAINT tmp2_a_fkey;
ALTER TABLE tmp2 ADD FOREIGN KEY (a) REFERENCES gtemp_pk_rel ON DELETE CASCADE;
DELETE FROM gtemp_pk_rel WHERE a = 1;
SELECT * FROM tmp2;
DROP TABLE perm_pk_rel, temp_pk_rel, tmp2;

-- Test ALTER TABLE ... SET TABLESPACE -- reltablespace changes locally and globally
CREATE GLOBAL TEMP TABLE tmp2 (a int);
INSERT INTO tmp2 VALUES (1);
SELECT * FROM tmp2;
SELECT c.reltablespace AS global_tablespace,
       t.reltablespace AS local_tablespace,
       regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g')
  FROM pg_class c
  LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp2';
ALTER TABLE tmp2 SET TABLESPACE regress_tblspace;
SELECT * FROM tmp2;
SELECT s1.spcname AS global_tablespace, s2.spcname AS local_tablespace,
       regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g')
  FROM pg_class c
  JOIN pg_tablespace s1 ON s1.oid = c.reltablespace
  LEFT JOIN pg_temp_class t ON t.oid = c.oid
  JOIN pg_tablespace s2 ON s2.oid = t.reltablespace
 WHERE c.relname = 'tmp2';
DROP TABLE tmp2;

-- Test dependency on tablespace
SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_temp_test_tablespace LOCATION '';
CREATE GLOBAL TEMP TABLE tmp2 (a int) TABLESPACE regress_temp_test_tablespace;
\c
SET search_path = global_temp_tests;
DROP TABLESPACE regress_temp_test_tablespace; -- fail
DROP TABLE tmp2;
DROP TABLESPACE regress_temp_test_tablespace;

SET allow_in_place_tablespaces = true;
CREATE TABLESPACE regress_temp_test_tablespace LOCATION '';
CREATE GLOBAL TEMP TABLE tmp2 (a int);
ALTER TABLE tmp2 SET TABLESPACE regress_temp_test_tablespace;
\c
SET search_path = global_temp_tests;
DROP TABLESPACE regress_temp_test_tablespace; -- fail
DROP TABLE tmp2;
DROP TABLESPACE regress_temp_test_tablespace;

-- Test TRUNCATE
INSERT INTO tmp1 VALUES (1, 'xxx');
BEGIN;
TRUNCATE tmp1;
SELECT * FROM tmp1;
ROLLBACK;
SELECT * FROM tmp1;

BEGIN;
SAVEPOINT sp1;
TRUNCATE tmp1;
SELECT * FROM tmp1;
RELEASE sp1;
SELECT * FROM tmp1;
ROLLBACK;
SELECT * FROM tmp1;

BEGIN;
SAVEPOINT sp1;
TRUNCATE tmp1;
SELECT * FROM tmp1;
ROLLBACK TO sp1;
SELECT * FROM tmp1;
COMMIT;
SELECT * FROM tmp1;

TRUNCATE tmp1;
SELECT * FROM tmp1;

-- Test CLUSTER -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1' \gset
CLUSTER tmp1 USING tmp1_pkey;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1';

-- CLUSTER not allowed on pg_temp_class
CLUSTER pg_temp_class; -- fail

-- Test REPACK -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1' \gset
REPACK tmp1;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1';

-- REPACK not allowed on pg_temp_class
REPACK pg_temp_class; -- fail

-- Test VACUUM FULL -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1' \gset
VACUUM FULL tmp1;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c LEFT JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.relname = 'tmp1';

-- VACUUM FULL not allowed on pg_temp_class
VACUUM FULL pg_temp_class; -- silently ignored

-- Test pg_relation_filenode() now matches local relfilenode
SELECT relfilenode = pg_relation_filenode('tmp1'::regclass) AS ok
  FROM pg_temp_class WHERE oid = 'tmp1'::regclass;

-- VACUUM initializes toast tables
\c
SET search_path = global_temp_tests;
SELECT EXISTS (SELECT 1 FROM pg_class t WHERE t.oid = c.reltoastrelid),
       EXISTS (SELECT 1 FROM pg_temp_class t WHERE t.oid = c.reltoastrelid)
FROM pg_class c
WHERE oid = 'tmp1'::regclass;

VACUUM tmp1;
SELECT EXISTS (SELECT 1 FROM pg_class t WHERE t.oid = c.reltoastrelid),
       EXISTS (SELECT 1 FROM pg_temp_class t WHERE t.oid = c.reltoastrelid)
FROM pg_class c
WHERE oid = 'tmp1'::regclass;

\c
SET search_path = global_temp_tests;
VACUUM FULL tmp1;
SELECT EXISTS (SELECT 1 FROM pg_class t WHERE t.oid = c.reltoastrelid),
       EXISTS (SELECT 1 FROM pg_temp_class t WHERE t.oid = c.reltoastrelid)
FROM pg_class c
WHERE oid = 'tmp1'::regclass;

-- Test subtransaction rollback of pending pg_temp_class inserts
\c
SET search_path = global_temp_tests;
BEGIN;
SELECT count(*) FROM tmp1;
SAVEPOINT sp;
DROP TABLE tmp1;
ROLLBACK TO sp;
INSERT INTO tmp1 VALUES (1, 'xxx');
COMMIT;
SELECT oid::regclass FROM pg_temp_class ORDER BY 1;
SELECT * FROM tmp1;

-- Test stats updates applied by CREATE INDEX, ANALYZE, VACUUM, and REPACK
CREATE GLOBAL TEMP TABLE tmp2 (a int);
INSERT INTO tmp2 SELECT * FROM generate_series(1, 100);
SELECT c.oid::regclass,
       c.relpages AS global_relpages, c.reltuples AS global_reltuples,
       CASE WHEN t.relpages = 0 THEN 'zero' ELSE 'non-zero' END AS local_relpages,
       t.reltuples AS local_reltuples
  FROM pg_class c JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.oid = 'tmp2'::regclass;

CREATE INDEX tmp2_a_idx ON tmp2(a);
SELECT c.oid::regclass,
       c.relpages AS global_relpages, c.reltuples AS global_reltuples,
       CASE WHEN t.relpages = 0 THEN 'zero' ELSE 'non-zero' END AS local_relpages,
       t.reltuples AS local_reltuples
  FROM pg_class c JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.oid = 'tmp2'::regclass OR c.oid = 'tmp2_a_idx'::regclass ORDER BY 1;

INSERT INTO tmp2 SELECT * FROM generate_series(101, 300);
ANALYZE tmp2;
SELECT c.oid::regclass,
       c.relpages AS global_relpages, c.reltuples AS global_reltuples,
       CASE WHEN t.relpages = 0 THEN 'zero' ELSE 'non-zero' END AS local_relpages,
       t.reltuples AS local_reltuples
  FROM pg_class c JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.oid = 'tmp2'::regclass OR c.oid = 'tmp2_a_idx'::regclass ORDER BY 1;

DELETE FROM tmp2 WHERE a % 2 = 0;
VACUUM ANALYZE tmp2;
SELECT c.oid::regclass,
       c.relpages AS global_relpages, c.reltuples AS global_reltuples,
       CASE WHEN t.relpages = 0 THEN 'zero' ELSE 'non-zero' END AS local_relpages,
       t.reltuples AS local_reltuples
  FROM pg_class c JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.oid = 'tmp2'::regclass OR c.oid = 'tmp2_a_idx'::regclass ORDER BY 1;

DELETE FROM tmp2 WHERE a % 3 = 0;
REPACK (ANALYZE) tmp2;
SELECT c.oid::regclass,
       c.relpages AS global_relpages, c.reltuples AS global_reltuples,
       CASE WHEN t.relpages = 0 THEN 'zero' ELSE 'non-zero' END AS local_relpages,
       t.reltuples AS local_reltuples
  FROM pg_class c JOIN pg_temp_class t ON t.oid = c.oid
 WHERE c.oid = 'tmp2'::regclass OR c.oid = 'tmp2_a_idx'::regclass ORDER BY 1;

-- Test stats usage
CREATE FUNCTION row_estimate(query text) RETURNS int
LANGUAGE plpgsql AS
$$
DECLARE
  line text;
BEGIN
  FOR line IN EXECUTE FORMAT('EXPLAIN %s', query)
  LOOP
    RETURN (regexp_match(line, 'rows=(\d*)'))[1]::int;
  END LOOP;
END;
$$;

SELECT row_estimate('SELECT * FROM tmp2');

-- Test in-place stats update (non-transactional)
TRUNCATE tmp2;
SELECT reltuples FROM pg_temp_class WHERE oid = 'tmp2'::regclass;
SELECT row_estimate('SELECT * FROM tmp2');

BEGIN;
INSERT INTO tmp2 SELECT * FROM generate_series(1, 100);
ANALYZE tmp2;
SELECT reltuples FROM pg_temp_class WHERE oid = 'tmp2'::regclass;
SELECT row_estimate('SELECT * FROM tmp2');
ROLLBACK;

SELECT reltuples FROM pg_temp_class WHERE oid = 'tmp2'::regclass;
SELECT row_estimate('SELECT * FROM tmp2');

-- Test in-place stats update after regular update (transactional)
BEGIN;
TRUNCATE tmp2;
INSERT INTO tmp2 SELECT * FROM generate_series(1, 50);
ANALYZE tmp2;
SELECT reltuples FROM pg_temp_class WHERE oid = 'tmp2'::regclass;
SELECT row_estimate('SELECT * FROM tmp2');
ROLLBACK;

SELECT reltuples FROM pg_temp_class WHERE oid = 'tmp2'::regclass;
SELECT row_estimate('SELECT * FROM tmp2');

-- Test manually updating stats
SELECT pg_clear_relation_stats('global_temp_tests', 'tmp2');
SELECT oid::regclass, relpages, reltuples, relallvisible, relallfrozen
  FROM pg_class WHERE oid = 'tmp2'::regclass;
SELECT oid::regclass, relpages, reltuples, relallvisible, relallfrozen
  FROM pg_temp_class WHERE oid = 'tmp2'::regclass;

SELECT pg_restore_relation_stats(
  'schemaname', 'global_temp_tests',
  'relname', 'tmp2',
  'relpages', 5,
  'reltuples', 150::real,
  'relallvisible', 10,
  'relallfrozen', 20);
SELECT oid::regclass, relpages, reltuples, relallvisible, relallfrozen
  FROM pg_class WHERE oid = 'tmp2'::regclass;
SELECT oid::regclass, relpages, reltuples, relallvisible, relallfrozen
  FROM pg_temp_class WHERE oid = 'tmp2'::regclass;

DROP TABLE tmp2;

-- Test view creation
CREATE VIEW v AS SELECT * FROM tmp1;
SELECT * FROM v;
DROP VIEW v;

CREATE TEMP VIEW v AS SELECT * FROM tmp1;
SELECT * FROM v;
DROP VIEW v;

CREATE GLOBAL TEMP VIEW v AS SELECT * FROM tmp1; -- fail

-- Test global temp sequence
CREATE GLOBAL TEMP SEQUENCE s MINVALUE 100 MAXVALUE 130 INCREMENT 10 START WITH 110 CYCLE;
\d s
SELECT nextval('s') FROM generate_series(1, 5);
\c
SET search_path = global_temp_tests;
SELECT nextval('s') FROM generate_series(1, 5);

-- Test that sequence initialization survives ROLLBACK
\c
SET search_path = global_temp_tests;
BEGIN;
SELECT nextval('s') FROM generate_series(1, 2);
ROLLBACK;
SELECT nextval('s') FROM generate_series(1, 2);

\c
SET search_path = global_temp_tests;
BEGIN;
SAVEPOINT sp;
SELECT nextval('s') FROM generate_series(1, 2);
ROLLBACK TO sp;
SELECT nextval('s') FROM generate_series(1, 3);
ROLLBACK;
SELECT nextval('s') FROM generate_series(1, 2);

-- Test lastval() after ROLLBACK of sequence initialization
\c
SET search_path = global_temp_tests;
BEGIN;
SELECT nextval('s') FROM generate_series(1, 2);
ROLLBACK;
SELECT lastval();

\c
SET search_path = global_temp_tests;
BEGIN;
SAVEPOINT sp;
SELECT nextval('s') FROM generate_series(1, 2);
ROLLBACK TO sp;
SELECT lastval();
ROLLBACK;
SELECT lastval();
SELECT nextval('s') FROM generate_series(1, 2);

-- Test tempfrozenxid/tempminmxid update on commit
\c
SET search_path = global_temp_tests;
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();

BEGIN;
CREATE GLOBAL TEMP TABLE tmp2 (a int);
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();
ROLLBACK;
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();

BEGIN;
SAVEPOINT sp;
CREATE GLOBAL TEMP TABLE tmp2 (a int);
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();
ROLLBACK TO sp;
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();
COMMIT;
SELECT tempfrozenxid, tempminmxid FROM pg_stat_activity WHERE pid = pg_backend_pid();

CREATE GLOBAL TEMP TABLE tmp2 (a int);
SELECT tempfrozenxid::text::bigint - (SELECT min(relfrozenxid::text::bigint)
                                        FROM pg_temp_class
                                       WHERE relfrozenxid::text != '0'),
       tempminmxid::text::bigint - (SELECT min(relminmxid::text::bigint)
                                      FROM pg_temp_class
                                     WHERE relminmxid::text != '0')
  FROM pg_stat_activity
 WHERE pid = pg_backend_pid();

VACUUM FREEZE pg_temp_class;
BEGIN;
SAVEPOINT sp;
DROP TABLE tmp2;
ROLLBACK TO SAVEPOINT sp;
COMMIT;

SELECT tempfrozenxid::text::bigint - (SELECT min(relfrozenxid::text::bigint)
                                        FROM pg_temp_class
                                       WHERE relfrozenxid::text != '0'),
       tempminmxid::text::bigint - (SELECT min(relminmxid::text::bigint)
                                      FROM pg_temp_class
                                     WHERE relminmxid::text != '0')
  FROM pg_stat_activity
 WHERE pid = pg_backend_pid();

DROP TABLE tmp2;
