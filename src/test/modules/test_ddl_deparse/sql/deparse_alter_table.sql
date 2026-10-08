--
-- Corner cases for DDL deparsing of ALTER TABLE.
--
-- The module intercepts each supported command and replays its deparsed
-- reconstruction, as described in deparse_create_table.sql.  As there,
-- ordinary syntax is left to 001_deparse_regress.pl, which replays the core
-- regression suite through the deparser.  What is tested here is the handful
-- of rules a replay cannot see, because the catalog state it arrives at is
-- the same either way, and the subcommands the core tests never write.
--
-- The event triggers created by the test_ddl_deparse script, first in the
-- schedule, are still installed.  Their "DDL test:" NOTICEs appear below.
--
LOAD 'test_ddl_deparse';
SET test_ddl_deparse.execute_deparsed_ddl = on;
SET test_ddl_deparse.print_deparsed_ddl = text;

CREATE SCHEMA deparse_at;

-- Several subcommands the user wrote stay in one ALTER TABLE, which preserves
-- the single table rewrite of the original.
CREATE TABLE deparse_at.t (a int, b text, c int);
ALTER TABLE deparse_at.t ADD COLUMN d int, ALTER COLUMN b SET NOT NULL, DROP COLUMN c;

-- A constraint the user did not name comes back unnamed, so that the replay
-- derives the name the way the original did.  The derived name is the same
-- either way, so the catalogs match whether or not the name is emitted.  This
-- covers a constraint written with a column definition too, which parse
-- analysis splits out into a subcommand of its own.
CREATE TABLE deparse_at.con (a int, b int);
CREATE TABLE deparse_at.ref (x int PRIMARY KEY);
ALTER TABLE deparse_at.con ADD CHECK (b > 0) NOT VALID;
ALTER TABLE deparse_at.con ADD FOREIGN KEY (a) REFERENCES deparse_at.ref (x)
	ON DELETE CASCADE;
ALTER TABLE deparse_at.con ADD COLUMN d int CHECK (d > 0);
ALTER TABLE deparse_at.con ADD COLUMN e int CONSTRAINT named_e CHECK (e > 0);
SELECT conname, pg_get_constraintdef(oid)
  FROM pg_constraint
  WHERE conrelid = 'deparse_at.con'::regclass
  ORDER BY conname COLLATE "C";

-- ADD NOT NULL for a column that already has one merges with the existing
-- constraint and creates no catalog row, so there is nothing to recover under
-- a name the user did not give.  The subcommand must still be emitted, not
-- dropped, which would leave the statement with no subcommand at all.  The
-- core tests have no such statement.
CREATE TABLE deparse_at.nnm (a int NOT NULL);
ALTER TABLE deparse_at.nnm ADD NOT NULL a;
CREATE TABLE deparse_at.nnmp (a int NOT NULL);
CREATE TABLE deparse_at.nnmc () INHERITS (deparse_at.nnmp);
ALTER TABLE deparse_at.nnmc ADD NOT NULL a;

-- An ALTER COLUMN TYPE whose USING references a column that a sibling
-- subcommand drops.  The USING is rendered to text at prep time, before the
-- drop, so it can still name the column.  Both statement orders behave
-- identically, as execution reorders subcommands by pass.  The core tests
-- never combine the two.
CREATE TABLE deparse_at.u (a int, b int);
INSERT INTO deparse_at.u VALUES (1, 42);
ALTER TABLE deparse_at.u ALTER COLUMN a TYPE text USING b::text, DROP COLUMN b;
CREATE TABLE deparse_at.u2 (a int, b int);
INSERT INTO deparse_at.u2 VALUES (1, 42);
ALTER TABLE deparse_at.u2 DROP COLUMN b, ALTER COLUMN a TYPE text USING b::text;

-- SET STATISTICS by column number, for an unnamed expression column of an
-- index reached via ALTER TABLE naming that index.  The subcommand carries
-- the column position instead of a name.  The core tests reach such a column
-- only through ALTER INDEX, which is not deparsed.
CREATE TABLE deparse_at.ei (a int, b int);
CREATE INDEX deparse_at_ei ON deparse_at.ei ((a + b));
ALTER TABLE deparse_at.deparse_at_ei ALTER COLUMN 1 SET STATISTICS 1000;
ALTER TABLE deparse_at.deparse_at_ei ALTER COLUMN 1 SET STATISTICS DEFAULT;

-- ADD ... PRIMARY KEY USING INDEX with the deferrability clauses, which the
-- core tests never give.
CREATE TABLE deparse_at.ui3 (a int);
CREATE UNIQUE INDEX ui3_uq ON deparse_at.ui3 (a);
ALTER TABLE deparse_at.ui3
  ADD CONSTRAINT ui3_pk PRIMARY KEY USING INDEX ui3_uq DEFERRABLE INITIALLY DEFERRED;

-- CLUSTER ON a leaf partition's index.  That index has a pg_inherits row, so
-- the inheritance-child skip must not take it for a recursed child.
CREATE TABLE deparse_at.cl (a int NOT NULL) PARTITION BY RANGE (a);
CREATE TABLE deparse_at.cl1 PARTITION OF deparse_at.cl FOR VALUES FROM (0) TO (10);
CREATE INDEX cl_a_idx ON deparse_at.cl (a);
ALTER TABLE deparse_at.cl1 CLUSTER ON cl1_a_idx;

-- The ENABLE spellings of the trigger and rule subcommands.  The core tests
-- use none of these four.
CREATE TABLE deparse_at.tr (a int);
CREATE FUNCTION deparse_at.trigfn() RETURNS trigger LANGUAGE plpgsql
	AS 'BEGIN RETURN NEW; END';
CREATE TRIGGER trg BEFORE INSERT ON deparse_at.tr
	FOR EACH ROW EXECUTE FUNCTION deparse_at.trigfn();
CREATE RULE trg_rule AS ON INSERT TO deparse_at.tr DO INSTEAD NOTHING;
ALTER TABLE deparse_at.tr ENABLE REPLICA TRIGGER trg;
ALTER TABLE deparse_at.tr DISABLE TRIGGER ALL;
ALTER TABLE deparse_at.tr ENABLE TRIGGER ALL;
ALTER TABLE deparse_at.tr ENABLE TRIGGER USER;
ALTER TABLE deparse_at.tr ENABLE ALWAYS RULE trg_rule;

-- IF EXISTS on a missing table is a no-op: nothing is collected, so there is
-- nothing to deparse and no warning.
ALTER TABLE IF EXISTS deparse_at.nonesuch ADD COLUMN z int;

RESET test_ddl_deparse.print_deparsed_ddl;
RESET test_ddl_deparse.execute_deparsed_ddl;

DROP SCHEMA deparse_at CASCADE;
