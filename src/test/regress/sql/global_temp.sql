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
CREATE GLOBAL TEMP TABLE tmp1 (a int);
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

DROP SCHEMA global_temp_xxx CASCADE;
DROP SCHEMA global_temp_yyy CASCADE;

-- Basic tests
INSERT INTO tmp1 VALUES (1);
SELECT * FROM tmp1;
\c
SET search_path = global_temp_tests;
SELECT * FROM tmp1;

-- Test pg_gtr_info() and pg_gtrs_in_use()
\c
SET search_path = global_temp_tests;
SELECT * FROM pg_gtr_info('tmp1'::regclass);
SELECT * FROM pg_gtrs_in_use();

SELECT * FROM tmp1;

SELECT c.relfilenode = c.oid,
       pg_relation_filenode('tmp1'::regclass) = c.relfilenode,
       t.relfilenode = c.relfilenode,
       t.reltablespace = c.reltablespace
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp1'::regclass;

SELECT c.relname,
       t.relfilenode = c.relfilenode,
       t.reltablespace = c.reltablespace
  FROM pg_gtrs_in_use() t LEFT JOIN pg_class c ON c.oid = t.oid
 ORDER BY c.relname;

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
CREATE GLOBAL TEMP TABLE tmp2 (a int REFERENCES perm_pk_rel); -- fail
CREATE GLOBAL TEMP TABLE tmp2 (a int REFERENCES temp_pk_rel); -- fail
DROP TABLE perm_pk_rel, temp_pk_rel;

-- Test ALTER TABLE ... SET TABLESPACE -- reltablespace changes locally and globally
CREATE GLOBAL TEMP TABLE tmp2 (a int);
INSERT INTO tmp2 VALUES (1);
SELECT * FROM tmp2;
SELECT c.reltablespace AS global_tablespace,
       t.reltablespace AS local_tablespace,
       regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g')
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp2'::regclass;

ALTER TABLE tmp2 SET TABLESPACE regress_tblspace;
SELECT * FROM tmp2;
SELECT s1.spcname AS global_tablespace, s2.spcname AS local_tablespace,
       regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g')
  FROM pg_class c
  LEFT JOIN pg_tablespace s1 ON s1.oid = c.reltablespace,
  LATERAL pg_gtr_info(c.oid) t
  LEFT JOIN pg_tablespace s2 ON s2.oid = t.reltablespace
 WHERE c.oid = 'tmp2'::regclass;
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
INSERT INTO tmp1 VALUES (1);
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

-- Test REPACK -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp1'::regclass \gset

REPACK tmp1;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp1'::regclass;

-- Test VACUUM FULL -- relfilenode only changes locally
SELECT c.relfilenode AS global_relfilenode, t.relfilenode AS local_relfilenode
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp1'::regclass \gset

VACUUM FULL tmp1;
SELECT CASE WHEN c.relfilenode = :global_relfilenode THEN 'unchanged' ELSE 'changed' END AS global_relfilenode,
       CASE WHEN t.relfilenode = :local_relfilenode THEN 'unchange' ELSE 'changed' END AS local_relfilenode
  FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
 WHERE c.oid = 'tmp1'::regclass;

-- Test subtransaction rollback of DROP
\c
SET search_path = global_temp_tests;
BEGIN;
SELECT count(*) FROM tmp1;
SAVEPOINT sp;
DROP TABLE tmp1;
ROLLBACK TO sp;
INSERT INTO tmp1 VALUES (1);
COMMIT;

-- Re-check pg_gtrs_in_use()
SELECT c.relname,
       t.relfilenode = c.relfilenode,
       t.reltablespace = c.reltablespace
  FROM pg_gtrs_in_use() t LEFT JOIN pg_class c ON c.oid = t.oid
 ORDER BY c.relname;

-- Test view creation
CREATE VIEW v AS SELECT * FROM tmp1;
SELECT * FROM v;
DROP VIEW v;

CREATE TEMP VIEW v AS SELECT * FROM tmp1;
SELECT * FROM v;
DROP VIEW v;

CREATE GLOBAL TEMP VIEW v AS SELECT * FROM tmp1; -- fail
