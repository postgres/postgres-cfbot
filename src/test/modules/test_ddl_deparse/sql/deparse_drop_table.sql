--
-- Corner cases for DDL deparsing of DROP TABLE.
--
-- See deparse_create_table.sql.
--
LOAD 'test_ddl_deparse';
SET test_ddl_deparse.execute_deparsed_ddl = on;
SET test_ddl_deparse.print_deparsed_ddl = text;

CREATE SCHEMA deparse_dt;
SET search_path = deparse_dt, public;

-- The dropped tables are emitted with their schema
CREATE TABLE d1 (a int);
CREATE TABLE d2 (a int);
DROP TABLE d1, deparse_dt.d2;

-- A table that IF EXISTS didn't find is not emitted, and if it found none,
-- there is nothing to deparse
CREATE TABLE d3 (a int);
DROP TABLE IF EXISTS d3, nonesuch;
DROP TABLE IF EXISTS nonesuch;

-- CASCADE is emitted as written, and the objects it dropped are not
CREATE TABLE d4 (a int);
CREATE VIEW v4 AS SELECT * FROM d4;
DROP TABLE d4 CASCADE;

-- Nor are the partitions dropped with their parent
CREATE TABLE p (a int) PARTITION BY LIST (a);
CREATE TABLE p1 PARTITION OF p FOR VALUES IN (1);
DROP TABLE p;

-- A temporary table is emitted in pg_temp, and names are quoted as needed
CREATE TEMP TABLE tt (a int);
CREATE TABLE "D 5" (a int);
DROP TABLE tt, "D 5";

-- DROP of other relation kinds is not supported
CREATE TABLE d6 (a int);
CREATE INDEX d6_a ON d6 (a);
DROP INDEX d6_a;

SELECT relname FROM pg_class
  WHERE relnamespace = 'deparse_dt'::regnamespace ORDER BY relname;

RESET test_ddl_deparse.print_deparsed_ddl;
RESET test_ddl_deparse.execute_deparsed_ddl;
RESET search_path;

DROP SCHEMA deparse_dt CASCADE;
