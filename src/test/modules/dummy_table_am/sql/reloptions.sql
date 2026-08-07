-- Tests for the table AM amoptions callback and add_reloption_to_kind()
CREATE EXTENSION dummy_table_am;

-- Sanity: CREATE TABLE with AM-specific options succeeds and round-trips
CREATE TABLE dummy_t (a int) USING dummy_table_am
    WITH (option_int = 17, option_real = 2.5, option_bool = false,
          option_enum = 'two', fillfactor = 60);
SELECT reloptions FROM pg_class
    WHERE oid = 'dummy_t'::regclass ORDER BY reloptions;

-- AM-specific option ranges are enforced (option_int allows -10..100)
CREATE TABLE dummy_oor (a int) USING dummy_table_am WITH (option_int = 9999);

-- Unknown options are rejected at CREATE TABLE time
CREATE TABLE dummy_bad (a int) USING dummy_table_am WITH (parallel_workers = 4);

-- Default values land in pg_class only when the user did not set them
CREATE TABLE dummy_defaults (a int) USING dummy_table_am;
SELECT reloptions FROM pg_class WHERE oid = 'dummy_defaults'::regclass;
DROP TABLE dummy_defaults;

-- ALTER TABLE ... SET (...) with AM-specific option
ALTER TABLE dummy_t SET (option_int = 42);
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;

-- ALTER TABLE ... SET (...) with an unknown option errors
ALTER TABLE dummy_t SET (parallel_workers = 4);

-- ALTER TABLE ... RESET (option) round-trips
ALTER TABLE dummy_t RESET (option_int);
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;

-- SET ACCESS METHOD to an AM with no amoptions of its own (heap) must
-- still revalidate: an option that only the *current* AM understands
-- (option_int) must not be silently left behind when switching away.
ALTER TABLE dummy_t RESET (option_real, option_bool, option_enum);
ALTER TABLE dummy_t SET (option_int = 42);
ALTER TABLE dummy_t SET ACCESS METHOD heap;
-- Confirm nothing changed
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'dummy_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;
-- Succeeds once the offending option is cleared in the same statement.
ALTER TABLE dummy_t SET ACCESS METHOD heap, RESET (option_int);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'dummy_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;

-- SET ACCESS METHOD revalidation:
--   moving a heap table that has standard heap options not accepted by the
--   new AM (parallel_workers) into dummy_table_am must fail with a clear
--   message and must NOT silently drop the option.
CREATE TABLE heap_t (a int) WITH (fillfactor = 70, parallel_workers = 4);
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;
ALTER TABLE heap_t SET ACCESS METHOD dummy_table_am;
-- Confirm nothing changed
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;

-- After RESETing the offending option in the same statement the swap
-- succeeds; fillfactor survives because dummy_table_am inherits it via
-- add_reloption_to_kind().
ALTER TABLE heap_t SET ACCESS METHOD dummy_table_am, RESET (parallel_workers);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;

-- Going back to heap still works: heap accepts fillfactor.
ALTER TABLE heap_t SET ACCESS METHOD heap;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;

-- SET ACCESS METHOD + SET (...) of an option that only the new AM accepts.
CREATE TABLE heap_to_dt (a int);
ALTER TABLE heap_to_dt SET ACCESS METHOD dummy_table_am, SET (option_int = 25);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;

-- Partitioned-table inheritance: AM declared on the parent partition flows
-- to partitions that don't override it.  Partitioned tables themselves
-- cannot carry reloptions; the test verifies the AM lookup that
-- DefineRelation does for partitions.
CREATE TABLE parted (a int) PARTITION BY RANGE (a) USING dummy_table_am;
CREATE TABLE parted_p1 PARTITION OF parted FOR VALUES FROM (0) TO (100)
    WITH (option_int = 11);
SELECT c.relname,
       (SELECT amname FROM pg_am WHERE oid = c.relam) AS amname,
       c.reloptions
    FROM pg_class c
    WHERE c.oid IN ('parted'::regclass, 'parted_p1'::regclass)
    ORDER BY c.relname;

-- A partition that explicitly chooses heap must reject options that are
-- only known to the parent's AM.
CREATE TABLE parted_p2 PARTITION OF parted FOR VALUES FROM (100) TO (200)
    USING heap WITH (option_int = 9);

DROP TABLE parted;
DROP TABLE heap_to_dt;
DROP TABLE heap_t;
DROP TABLE dummy_t;

DROP EXTENSION dummy_table_am;
