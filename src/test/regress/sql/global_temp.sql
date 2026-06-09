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

DROP SCHEMA global_temp_xxx CASCADE;
DROP SCHEMA global_temp_yyy CASCADE;

-- Basic tests
INSERT INTO tmp1 VALUES (1, 'xxx');
SELECT * FROM tmp1;
\c
SET search_path = global_temp_tests;
SELECT * FROM tmp1;

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
DROP INDEX CONCURRENTLY tmp1_b_idx;

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

-- Test ALTER TABLE ... SET TABLESPACE
CREATE GLOBAL TEMP TABLE tmp2 (a int);
INSERT INTO tmp2 VALUES (1);
SELECT * FROM tmp2;
SELECT regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g');
ALTER TABLE tmp2 SET TABLESPACE regress_tblspace;
SELECT * FROM tmp2;
SELECT regexp_replace(pg_relation_filepath('tmp2'), '(\d+)', 'NNN', 'g');
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

-- Test view creation
INSERT INTO tmp1 VALUES (1, 'xxx');
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
