-- Tests for the table AM amoptions callback and add_reloption_to_kind()
CREATE EXTENSION dummy_table_am;

-- AM-specific and standard options are accepted
CREATE TABLE dummy_t (a int) USING dummy_table_am
    WITH (option_int = 17, option_real = 2.5, option_bool = false,
          option_enum = 'two', fillfactor = 60);
SELECT reloptions FROM pg_class
    WHERE oid = 'dummy_t'::regclass ORDER BY reloptions;

-- Out-of-range and unknown options are rejected
CREATE TABLE dummy_oor (a int) USING dummy_table_am WITH (option_int = 9999);
CREATE TABLE dummy_bad (a int) USING dummy_table_am WITH (no_such_option = 4);

-- Inherited options are standard ones, the AM's own are not
SELECT pg_reloption_is_standard('fillfactor'),
       pg_reloption_is_standard('option_int');

-- Defaults are not stored in pg_class
CREATE TABLE dummy_defaults (a int) USING dummy_table_am;
SELECT reloptions FROM pg_class WHERE oid = 'dummy_defaults'::regclass;
DROP TABLE dummy_defaults;

-- ALTER TABLE SET and RESET
ALTER TABLE dummy_t SET (option_int = 42);
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;
ALTER TABLE dummy_t SET (no_such_option = 4);
ALTER TABLE dummy_t RESET (option_int);
SELECT reloptions FROM pg_class WHERE oid = 'dummy_t'::regclass;

-- SET ACCESS METHOD keeps standard options the new AM accepts
CREATE TABLE heap_t (a int) WITH (fillfactor = 70, autovacuum_vacuum_threshold = 4);
ALTER TABLE heap_t SET ACCESS METHOD dummy_table_am;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;

-- ... and fails if the new AM doesn't accept them, unless they're reset
ALTER TABLE heap_t SET ACCESS METHOD dummy_custom_table_am;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;
ALTER TABLE heap_t SET ACCESS METHOD dummy_custom_table_am,
    RESET (fillfactor, autovacuum_vacuum_threshold);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_t'::regclass;

ALTER TABLE heap_t SET ACCESS METHOD heap;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_t'::regclass;

-- SET ACCESS METHOD with SET of an option only the new AM accepts
CREATE TABLE heap_to_dt (a int);
ALTER TABLE heap_to_dt SET ACCESS METHOD dummy_table_am, SET (option_int = 25);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;

-- Switching to heap fails while an option heap doesn't accept is set
ALTER TABLE heap_to_dt SET ACCESS METHOD heap;
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;
ALTER TABLE heap_to_dt SET ACCESS METHOD heap, RESET (option_int);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;

-- Only the final options are checked, so a RESET may follow a SET
ALTER TABLE heap_to_dt SET ACCESS METHOD dummy_table_am, SET (option_int = 25);
ALTER TABLE heap_to_dt SET ACCESS METHOD heap, SET (option_real = 1),
    RESET (option_int);
ALTER TABLE heap_to_dt SET ACCESS METHOD heap, SET (fillfactor = 50),
    RESET (option_int);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;

-- Option values are still checked
ALTER TABLE heap_to_dt SET ACCESS METHOD dummy_table_am, SET (option_int = 9999);
SELECT amname FROM pg_class c JOIN pg_am a ON a.oid = c.relam
    WHERE c.oid = 'heap_to_dt'::regclass;
SELECT reloptions FROM pg_class WHERE oid = 'heap_to_dt'::regclass;

-- fillfactor takes effect
CREATE TABLE dummy_ff10 (a int) USING dummy_table_am WITH (fillfactor = 10);
CREATE TABLE dummy_ff100 (a int) USING dummy_table_am WITH (fillfactor = 100);
INSERT INTO dummy_ff10 SELECT generate_series(1, 5000);
INSERT INTO dummy_ff100 SELECT generate_series(1, 5000);
VACUUM dummy_ff10;
VACUUM dummy_ff100;
SELECT (SELECT relpages FROM pg_class WHERE oid = 'dummy_ff10'::regclass) >
       (SELECT relpages FROM pg_class WHERE oid = 'dummy_ff100'::regclass)
       AS low_fillfactor_uses_more_pages;
DROP TABLE dummy_ff10;
DROP TABLE dummy_ff100;

-- Tables with a TOAST table, with and without options
CREATE TABLE dummy_txt (a int, b text) USING dummy_table_am;
INSERT INTO dummy_txt VALUES (1, repeat('x', 10000));
SELECT a, length(b) FROM dummy_txt;
DROP TABLE dummy_txt;

CREATE TABLE dummy_txt_opt (a int, b text)
    USING dummy_table_am WITH (option_int = 7);
INSERT INTO dummy_txt_opt VALUES (1, repeat('x', 10000));
SELECT a, length(b) FROM dummy_txt_opt;
DROP TABLE dummy_txt_opt;

-- A partition's options are checked by the AM it inherits from its parent
CREATE TABLE parted (a int) PARTITION BY RANGE (a) USING dummy_table_am;
CREATE TABLE parted_p1 PARTITION OF parted FOR VALUES FROM (0) TO (100)
    WITH (option_int = 11);
SELECT c.relname,
       (SELECT amname FROM pg_am WHERE oid = c.relam) AS amname,
       c.reloptions
    FROM pg_class c
    WHERE c.oid IN ('parted'::regclass, 'parted_p1'::regclass)
    ORDER BY c.relname;

-- ... or the one given in USING
CREATE TABLE parted_p2 PARTITION OF parted FOR VALUES FROM (100) TO (200)
    USING heap WITH (option_int = 9);

-- ... or default_table_access_method, if the parent has no AM
SET default_table_access_method = dummy_table_am;
CREATE TABLE parted_noam (a int) PARTITION BY RANGE (a);
CREATE TABLE parted_noam_p1 PARTITION OF parted_noam
    FOR VALUES FROM (0) TO (100) WITH (option_int = 12);
SELECT c.relname,
       (SELECT amname FROM pg_am WHERE oid = c.relam) AS amname,
       c.reloptions
    FROM pg_class c
    WHERE c.oid IN ('parted_noam'::regclass, 'parted_noam_p1'::regclass)
    ORDER BY c.relname;
RESET default_table_access_method;
DROP TABLE parted_noam;

DROP TABLE parted;
DROP TABLE heap_to_dt;
DROP TABLE heap_t;
DROP TABLE dummy_t;

DROP EXTENSION dummy_table_am;
