--
-- Tests for DDL deparsing of ALTER TABLE.
--
-- As with deparse_create_table, the module intercepts each supported
-- ALTER TABLE, runs it in a rolled-back subtransaction, deparses the
-- collected command, and replays the reconstruction; the deparsed text is
-- shown as a NOTICE.  A single ALTER TABLE -- possibly carrying several
-- subcommands -- deparses to exactly one ALTER TABLE, preserving the
-- single-pass semantics of the original.
--
-- The event triggers created by the test_ddl_deparse script (first in the
-- schedule) remain installed, so their "DDL test:" NOTICEs appear below.
--
LOAD 'test_ddl_deparse';
SET test_ddl_deparse.execute_deparsed_ddl = on;
SET test_ddl_deparse.print_deparsed_ddl = text;

CREATE SCHEMA deparse_at;
CREATE TABLE deparse_at.t (a int, b text);

-- ADD / DROP COLUMN, including the IF [NOT] EXISTS guards
ALTER TABLE deparse_at.t ADD COLUMN c int;
ALTER TABLE deparse_at.t ADD COLUMN IF NOT EXISTS d text;
ALTER TABLE deparse_at.t DROP COLUMN b;
ALTER TABLE deparse_at.t DROP COLUMN IF EXISTS c;

-- several subcommands in one statement stay in one statement (collected in
-- execution-pass order, which replays to the same result)
ALTER TABLE deparse_at.t ADD COLUMN e int, ADD COLUMN f text, DROP COLUMN d;

-- ADD COLUMN carrying a plain NOT NULL and/or DEFAULT is rendered inline by
-- the column deparse; the internal not-null constraint is not emitted twice
ALTER TABLE deparse_at.t ADD COLUMN g int NOT NULL DEFAULT 0;

-- ALTER COLUMN subcommands
CREATE TABLE deparse_at.c (a int, b text, n numeric);
ALTER TABLE deparse_at.c ALTER COLUMN a SET DEFAULT 42;
-- SET DEFAULT NULL: a bare NULL default is not stored in pg_attrdef, so it
-- must be rendered as written rather than looked up
ALTER TABLE deparse_at.c ALTER COLUMN a SET DEFAULT NULL;
ALTER TABLE deparse_at.c ALTER COLUMN a DROP DEFAULT;
ALTER TABLE deparse_at.c ALTER COLUMN a SET NOT NULL;
ALTER TABLE deparse_at.c ALTER COLUMN a DROP NOT NULL;
ALTER TABLE deparse_at.c ALTER COLUMN b SET STATISTICS 100;
-- SET STATISTICS DEFAULT leaves the value node NULL
ALTER TABLE deparse_at.c ALTER COLUMN b SET STATISTICS DEFAULT;
ALTER TABLE deparse_at.c ALTER COLUMN b SET (n_distinct = 5);
ALTER TABLE deparse_at.c ALTER COLUMN b RESET (n_distinct);
ALTER TABLE deparse_at.c ALTER COLUMN b SET STORAGE EXTERNAL;
ALTER TABLE deparse_at.c ALTER COLUMN b SET COMPRESSION pglz;
-- SET DATA TYPE, with and without a USING expression
ALTER TABLE deparse_at.c ALTER COLUMN a SET DATA TYPE bigint;
ALTER TABLE deparse_at.c ALTER COLUMN n SET DATA TYPE int USING n::int;
ALTER TABLE deparse_at.c ALTER COLUMN b TYPE varchar(20) COLLATE "C";
-- several ALTER COLUMN subcommands in one statement
ALTER TABLE deparse_at.c
	ALTER COLUMN a SET DEFAULT 1,
	ALTER COLUMN b SET NOT NULL,
	ALTER COLUMN n SET STATISTICS 50;

-- constraint subcommands
CREATE TABLE deparse_at.con (a int, b int, c int);
ALTER TABLE deparse_at.con ADD CONSTRAINT chk CHECK (a > 0);
ALTER TABLE deparse_at.con ADD CHECK (b > 0) NOT VALID;
ALTER TABLE deparse_at.con VALIDATE CONSTRAINT con_b_check;
ALTER TABLE deparse_at.con ADD PRIMARY KEY (a);
ALTER TABLE deparse_at.con ADD CONSTRAINT uq UNIQUE (b) WITH (fillfactor = 70);
ALTER TABLE deparse_at.con ADD UNIQUE (c) DEFERRABLE INITIALLY DEFERRED;
CREATE TABLE deparse_at.ref (x int PRIMARY KEY);
ALTER TABLE deparse_at.con ADD FOREIGN KEY (a) REFERENCES deparse_at.ref (x)
	ON DELETE CASCADE;
ALTER TABLE deparse_at.con DROP CONSTRAINT chk;
ALTER TABLE deparse_at.con DROP CONSTRAINT IF EXISTS nonesuch;
-- A constraint the user did not name must come back unnamed, so that the
-- replay derives the name the same way the original did.  Execution fills the
-- derived name into the very subcommand that is later collected, so the
-- question can only be answered from the raw statement -- including for a
-- constraint written with a column definition, which parse analysis splits out
-- into a subcommand of its own.
ALTER TABLE deparse_at.con ADD COLUMN d int CHECK (d > 0);
ALTER TABLE deparse_at.con ADD COLUMN e int CONSTRAINT named_e CHECK (e > 0);
-- exclusion constraint
CREATE TABLE deparse_at.ex (c circle);
ALTER TABLE deparse_at.ex ADD EXCLUDE USING gist (c WITH &&);

-- ALTER CONSTRAINT: deferrability and enforceability (rendered from the
-- ATAlterConstraint node; INITIALLY IMMEDIATE is the default and implicit)
CREATE TABLE deparse_at.acpk (x int PRIMARY KEY);
CREATE TABLE deparse_at.acfk (a int,
	CONSTRAINT fk FOREIGN KEY (a) REFERENCES deparse_at.acpk (x));
ALTER TABLE deparse_at.acfk ALTER CONSTRAINT fk DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE deparse_at.acfk ALTER CONSTRAINT fk DEFERRABLE INITIALLY IMMEDIATE;
ALTER TABLE deparse_at.acfk ALTER CONSTRAINT fk NOT DEFERRABLE;
ALTER TABLE deparse_at.acfk ALTER CONSTRAINT fk NOT ENFORCED;
ALTER TABLE deparse_at.acfk ALTER CONSTRAINT fk ENFORCED;

-- user-written table-level NOT NULL (named, unnamed, NO INHERIT).  The
-- internal NOT NULL that ADD COLUMN ... NOT NULL generates for a different
-- column in the same statement is rendered inline, not duplicated here; the
-- two are told apart by matching the raw statement per column.
CREATE TABLE deparse_at.nn (a int, b int, c int);
ALTER TABLE deparse_at.nn ADD CONSTRAINT nn_a NOT NULL a;
ALTER TABLE deparse_at.nn ADD NOT NULL b;
ALTER TABLE deparse_at.nn ADD COLUMN d int NOT NULL DEFAULT 0,
	ADD CONSTRAINT nn_c NOT NULL c;
CREATE TABLE deparse_at.nnp (a int);
CREATE TABLE deparse_at.nnc (a int) INHERITS (deparse_at.nnp);
ALTER TABLE ONLY deparse_at.nnp ADD CONSTRAINT nn_ni NOT NULL a NO INHERIT;
CREATE TABLE deparse_at.nnv (a int);
ALTER TABLE deparse_at.nnv ADD CONSTRAINT nn_nv NOT NULL a NOT VALID;
-- ADD NOT NULL for a column that already has one merges with the existing
-- constraint and creates no catalog row, so nothing can be looked up by the
-- name the user did not give.  The subcommand must still be emitted -- the
-- replay merges again, to the same effect -- rather than dropped, which would
-- leave the statement with no subcommand at all.
CREATE TABLE deparse_at.nnm (a int NOT NULL);
ALTER TABLE deparse_at.nnm ADD NOT NULL a;
CREATE TABLE deparse_at.nnmp (a int NOT NULL);
CREATE TABLE deparse_at.nnmc () INHERITS (deparse_at.nnmp);
ALTER TABLE deparse_at.nnmc ADD NOT NULL a;

-- table-level subcommands
CREATE ROLE regress_deparse_role;
CREATE TABLE deparse_at.tl (a int, b int);
ALTER TABLE deparse_at.tl OWNER TO regress_deparse_role;
ALTER TABLE deparse_at.tl SET UNLOGGED;
ALTER TABLE deparse_at.tl SET LOGGED;
ALTER TABLE deparse_at.tl SET WITHOUT OIDS;
ALTER TABLE deparse_at.tl SET (fillfactor = 70);
ALTER TABLE deparse_at.tl RESET (fillfactor);
ALTER TABLE deparse_at.tl SET TABLESPACE pg_default;
ALTER TABLE deparse_at.tl ENABLE ROW LEVEL SECURITY;
ALTER TABLE deparse_at.tl FORCE ROW LEVEL SECURITY;
ALTER TABLE deparse_at.tl NO FORCE ROW LEVEL SECURITY;
ALTER TABLE deparse_at.tl DISABLE ROW LEVEL SECURITY;
CREATE INDEX tl_a_idx ON deparse_at.tl (a);
ALTER TABLE deparse_at.tl CLUSTER ON tl_a_idx;
ALTER TABLE deparse_at.tl SET WITHOUT CLUSTER;
ALTER TABLE deparse_at.tl ALTER COLUMN a SET NOT NULL;
ALTER TABLE deparse_at.tl ADD UNIQUE (a);
ALTER TABLE deparse_at.tl REPLICA IDENTITY FULL;
ALTER TABLE deparse_at.tl REPLICA IDENTITY USING INDEX tl_a_key;
ALTER TABLE deparse_at.tl REPLICA IDENTITY DEFAULT;

-- inheritance
CREATE TABLE deparse_at.ip (a int, b int);
CREATE TABLE deparse_at.ic (a int, b int);
ALTER TABLE deparse_at.ic INHERIT deparse_at.ip;
ALTER TABLE deparse_at.ic NO INHERIT deparse_at.ip;

-- partition attach/detach
CREATE TABLE deparse_at.pt (a int) PARTITION BY RANGE (a);
CREATE TABLE deparse_at.pt1 (a int);
ALTER TABLE deparse_at.pt ATTACH PARTITION deparse_at.pt1 FOR VALUES FROM (1) TO (10);
ALTER TABLE deparse_at.pt DETACH PARTITION deparse_at.pt1;

-- CLUSTER ON a leaf-partition's index: the subcommand's collected address is
-- that index, and a leaf-partition index has a pg_inherits row, so the
-- inheritance-child skip must not mistake it for a recursed child and drop it
CREATE TABLE deparse_at.cl (a int NOT NULL) PARTITION BY RANGE (a);
CREATE TABLE deparse_at.cl1 PARTITION OF deparse_at.cl FOR VALUES FROM (0) TO (10);
CREATE INDEX cl_a_idx ON deparse_at.cl (a);
ALTER TABLE deparse_at.cl1 CLUSTER ON cl1_a_idx;

-- DROP EXPRESSION (of a generated column) and SET ACCESS METHOD
CREATE TABLE deparse_at.dx (a int, b int GENERATED ALWAYS AS (a * 2) STORED);
ALTER TABLE deparse_at.dx ALTER COLUMN b DROP EXPRESSION;
ALTER TABLE deparse_at.dx ALTER COLUMN b DROP EXPRESSION IF EXISTS;
ALTER TABLE deparse_at.dx SET ACCESS METHOD heap;
ALTER TABLE deparse_at.dx SET ACCESS METHOD DEFAULT;

-- ENABLE / DISABLE of triggers and rules, all spellings
CREATE FUNCTION deparse_at.trigfn() RETURNS trigger LANGUAGE plpgsql
	AS 'BEGIN RETURN NEW; END';
CREATE TRIGGER trg BEFORE INSERT ON deparse_at.dx
	FOR EACH ROW EXECUTE FUNCTION deparse_at.trigfn();
ALTER TABLE deparse_at.dx DISABLE TRIGGER trg;
ALTER TABLE deparse_at.dx ENABLE TRIGGER trg;
ALTER TABLE deparse_at.dx ENABLE ALWAYS TRIGGER trg;
ALTER TABLE deparse_at.dx ENABLE REPLICA TRIGGER trg;
ALTER TABLE deparse_at.dx DISABLE TRIGGER ALL;
ALTER TABLE deparse_at.dx ENABLE TRIGGER USER;
CREATE RULE trg_rule AS ON INSERT TO deparse_at.dx DO INSTEAD NOTHING;
ALTER TABLE deparse_at.dx DISABLE RULE trg_rule;
ALTER TABLE deparse_at.dx ENABLE RULE trg_rule;
ALTER TABLE deparse_at.dx ENABLE ALWAYS RULE trg_rule;
ALTER TABLE deparse_at.dx ENABLE REPLICA RULE trg_rule;

-- identity columns (ADD/SET/DROP GENERATED ... AS IDENTITY, with sequence
-- options that parse analysis splits out and the deparser restores from the
-- raw statement) and SET EXPRESSION of a generated column
CREATE TABLE deparse_at.idc (a int NOT NULL, b int NOT NULL,
	g int GENERATED ALWAYS AS (a + 1) STORED);
ALTER TABLE deparse_at.idc ALTER COLUMN a ADD GENERATED ALWAYS AS IDENTITY;
ALTER TABLE deparse_at.idc ALTER COLUMN b ADD GENERATED BY DEFAULT AS IDENTITY
	(START WITH 10 INCREMENT BY 2);
ALTER TABLE deparse_at.idc ALTER COLUMN a SET GENERATED BY DEFAULT;
ALTER TABLE deparse_at.idc ALTER COLUMN a RESTART WITH 100;
ALTER TABLE deparse_at.idc ALTER COLUMN b SET INCREMENT BY 5;
ALTER TABLE deparse_at.idc ALTER COLUMN a DROP IDENTITY;
ALTER TABLE deparse_at.idc ALTER COLUMN a DROP IDENTITY IF EXISTS;
ALTER TABLE deparse_at.idc ALTER COLUMN g SET EXPRESSION AS (a + 100);
-- ADD COLUMN and ADD IDENTITY of that same new column in one statement:
-- parse analysis collects it as two ALTER TABLE commands (the identity links
-- a sequence via a nested internal ALTER TABLE), which the deparser merges
-- back into the single ALTER TABLE the user wrote
ALTER TABLE deparse_at.idc
	ADD COLUMN c int NOT NULL,
	ALTER COLUMN c ADD GENERATED ALWAYS AS IDENTITY;

-- OF a composite type, and NOT OF
CREATE TYPE deparse_at.oftype AS (a int, b int);
CREATE TABLE deparse_at.oft (a int, b int);
ALTER TABLE deparse_at.oft OF deparse_at.oftype;
ALTER TABLE deparse_at.oft NOT OF;

-- RENAME and SET SCHEMA (collected as simple commands, not subcommands)
CREATE TABLE deparse_at.rn (a int, CONSTRAINT ck CHECK (a > 0));
ALTER TABLE deparse_at.rn RENAME COLUMN a TO b;
ALTER TABLE deparse_at.rn RENAME CONSTRAINT ck TO ck2;
ALTER TABLE deparse_at.rn RENAME TO rn2;
ALTER TABLE IF EXISTS deparse_at.nonesuch RENAME TO whatever;
CREATE SCHEMA deparse_at2;
ALTER TABLE deparse_at.rn2 SET SCHEMA deparse_at2;

-- ALTER COLUMN TYPE whose USING references a column dropped in the same
-- statement: the USING is rendered to text at prep time, before the drop, so
-- it can still name the column; the two subcommands (collected as one ALTER
-- TABLE) round-trip together.  Both statement orders behave identically, as
-- execution reorders subcommands by pass.
CREATE TABLE deparse_at.u (a int, b int);
INSERT INTO deparse_at.u VALUES (1, 42);
ALTER TABLE deparse_at.u ALTER COLUMN a TYPE text USING b::text, DROP COLUMN b;
CREATE TABLE deparse_at.u2 (a int, b int);
INSERT INTO deparse_at.u2 VALUES (1, 42);
ALTER TABLE deparse_at.u2 DROP COLUMN b, ALTER COLUMN a TYPE text USING b::text;

-- SET STATISTICS by column number: an unnamed expression column of an index,
-- reached via ALTER TABLE naming the index.  The subcommand carries the
-- column position, not a name (formerly a NULL-name crash); it deparses to
-- the same by-number form.
CREATE INDEX deparse_at_ei ON deparse_at.t ((a + e));
ALTER TABLE deparse_at.deparse_at_ei ALTER COLUMN 1 SET STATISTICS 1000;
ALTER TABLE deparse_at.deparse_at_ei ALTER COLUMN 1 SET STATISTICS DEFAULT;

-- ADD [CONSTRAINT name] {PRIMARY KEY | UNIQUE} USING INDEX adopts an existing
-- unique index.  It must round-trip as USING INDEX (deparsing the constraint
-- definition would build a duplicate index on replay).  The index's
-- pre-adoption name comes from the raw statement (adoption may rename the index
-- to the constraint name), so it survives to name the still-existing index on
-- replay.
CREATE TABLE deparse_at.ui1 (a int, b int);
CREATE UNIQUE INDEX ui1_uq ON deparse_at.ui1 (b);
-- named constraint, different index name: the index is renamed on replay too
ALTER TABLE deparse_at.ui1 ADD CONSTRAINT ui1_pk PRIMARY KEY USING INDEX ui1_uq;
-- unnamed UNIQUE over a covering (INCLUDE) index: constraint takes the index
-- name, no rename; the INCLUDE columns live in the index, not restated
CREATE TABLE deparse_at.ui2 (a int, b int, c int);
CREATE UNIQUE INDEX ui2_idx ON deparse_at.ui2 (a, b) INCLUDE (c);
ALTER TABLE deparse_at.ui2 ADD UNIQUE USING INDEX ui2_idx;
-- deferrable
CREATE TABLE deparse_at.ui3 (a int);
CREATE UNIQUE INDEX ui3_uq ON deparse_at.ui3 (a);
ALTER TABLE deparse_at.ui3
  ADD CONSTRAINT ui3_pk PRIMARY KEY USING INDEX ui3_uq DEFERRABLE INITIALLY DEFERRED;
-- combined with ALTER COLUMN TYPE in one statement (index rebuilt, name kept)
CREATE TABLE deparse_at.ui4 (f1 int);
CREATE UNIQUE INDEX ui4_i0 ON deparse_at.ui4 (f1);
ALTER TABLE deparse_at.ui4 ADD PRIMARY KEY USING INDEX ui4_i0, ALTER f1 TYPE bigint;

-- MERGE PARTITIONS / SPLIT PARTITION.  The source partition(s) are dropped by
-- the command, so their schema-qualified names are captured before the drop;
-- the created partition(s) survive, so their name and bound come from the
-- catalog by OID.  Names are always emitted schema-qualified regardless of how
-- the user wrote them, so the reconstruction replays under a restricted
-- search_path.
CREATE TABLE deparse_at.sales (id int, d date) PARTITION BY RANGE (d);
CREATE TABLE deparse_at.sales_jan PARTITION OF deparse_at.sales
  FOR VALUES FROM ('2022-01-01') TO ('2022-02-01');
CREATE TABLE deparse_at.sales_feb PARTITION OF deparse_at.sales
  FOR VALUES FROM ('2022-02-01') TO ('2022-03-01');
CREATE TABLE deparse_at.sales_mar PARTITION OF deparse_at.sales
  FOR VALUES FROM ('2022-03-01') TO ('2022-04-01');
-- merge two adjacent partitions (names unqualified, resolved via search_path)
SET search_path = deparse_at;
ALTER TABLE sales MERGE PARTITIONS (sales_jan, sales_feb) INTO sales_jan_feb;
RESET search_path;
-- split it back into two, with explicit bounds
ALTER TABLE deparse_at.sales SPLIT PARTITION deparse_at.sales_jan_feb INTO (
  PARTITION deparse_at.sales_jan FOR VALUES FROM ('2022-01-01') TO ('2022-02-01'),
  PARTITION deparse_at.sales_feb FOR VALUES FROM ('2022-02-01') TO ('2022-03-01'));
-- merged-into partition placed in a different schema than the parent
ALTER TABLE deparse_at.sales MERGE PARTITIONS (deparse_at.sales_feb, deparse_at.sales_mar)
  INTO deparse_at2.sales_feb_mar;
-- LIST partitioning
CREATE TABLE deparse_at.reg (r text) PARTITION BY LIST (r);
CREATE TABLE deparse_at.reg_w PARTITION OF deparse_at.reg FOR VALUES IN ('w');
CREATE TABLE deparse_at.reg_e PARTITION OF deparse_at.reg FOR VALUES IN ('e');
ALTER TABLE deparse_at.reg MERGE PARTITIONS (deparse_at.reg_w, deparse_at.reg_e)
  INTO deparse_at.reg_we;

-- IF EXISTS on a missing table is a no-op (no deparse, no warning)
ALTER TABLE IF EXISTS deparse_at.nonesuch ADD COLUMN z int;

SELECT attname FROM pg_attribute
  WHERE attrelid = 'deparse_at.t'::regclass AND attnum > 0 AND NOT attisdropped
  ORDER BY attnum;

DROP SCHEMA deparse_at CASCADE;
DROP SCHEMA deparse_at2 CASCADE;
DROP ROLE regress_deparse_role;
