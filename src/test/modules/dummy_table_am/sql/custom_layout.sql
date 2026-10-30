-- Tests for a table AM whose options struct does not begin with StdRdOptions
CREATE EXTENSION dummy_table_am;

-- Standard options are not accepted
CREATE TABLE dct_bad (a int) USING dummy_custom_table_am WITH (fillfactor = 50);

-- TOAST tables get the default toast_value_type.  option_c is at the offset
-- of StdRdOptions.toast_value_type; 0 is not a valid value for that, and 2
-- would select oid8.
CREATE TABLE dct_default (a int, b text) USING dummy_custom_table_am;
CREATE TABLE dct_c0 (a int, b text) USING dummy_custom_table_am
    WITH (option_c = 0);
CREATE TABLE dct_c2 (a int, b text) USING dummy_custom_table_am
    WITH (option_a = 10, option_b = 20, option_c = 2);
SELECT c.relname, c.reloptions, a.atttypid::regtype AS chunk_id_type
    FROM pg_class c
    JOIN pg_attribute a ON a.attrelid = c.reltoastrelid AND a.attname = 'chunk_id'
    WHERE c.relname IN ('dct_default', 'dct_c0', 'dct_c2')
    ORDER BY c.relname;

-- VACUUM of a table with TOAST data
INSERT INTO dct_c2
    SELECT 1, string_agg(md5(i::text), '') FROM generate_series(1, 500) i;
SELECT pg_relation_size(reltoastrelid) > 0 AS has_toast_data
    FROM pg_class WHERE oid = 'dct_c2'::regclass;
VACUUM dct_c2;
SELECT a, length(b) FROM dct_c2;

-- Switching to heap requires resetting the AM's options
ALTER TABLE dct_c2 SET ACCESS METHOD heap;
ALTER TABLE dct_c2 SET ACCESS METHOD heap, SET (fillfactor = 50),
    RESET (option_a, option_b, option_c);
SELECT (SELECT amname FROM pg_am WHERE oid = relam) AS amname, reloptions
    FROM pg_class WHERE oid = 'dct_c2'::regclass;
SELECT a, length(b) FROM dct_c2;

DROP TABLE dct_default;
DROP TABLE dct_c0;
DROP TABLE dct_c2;

DROP EXTENSION dummy_table_am;
