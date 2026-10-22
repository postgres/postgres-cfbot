-- Tests for zstd TOAST compression with dictionaries, and their lifecycle:
-- dictionaries belong to one column, values compressed with them never leave
-- that column in compressed form, a rewrite retires old dictionaries, and
-- dropping the column or table drops them.

SELECT NOT(enumvals @> '{zstd}') AS skip_test FROM pg_settings WHERE
  name = 'default_toast_compression' \gset
\if :skip_test
   \echo '*** skipping TOAST tests with zstd dictionaries (not supported) ***'
   \quit
\endif

CREATE SCHEMA zstd_dict;
SET search_path TO zstd_dict, public;

\set HIDE_TOAST_COMPRESSION false

-- Many small, similar JSON documents: the case dictionaries are made for.
-- Each is below the TOAST threshold uncompressed only with a dictionary.
CREATE FUNCTION doc(i int) RETURNS text LANGUAGE SQL AS $$
  SELECT format('{"customer_id": %s, "status": "active", "region": "us-east", '
                '"plan": "enterprise", "features": ["sso", "audit", "backup"], '
                '"tags": ["%s", "priority-%s"], "notes": "%s"}',
                i, md5(i::text), i % 5, repeat('lorem ipsum dolor sit amet ', 80))
$$;

CREATE TABLE cmdict (id int, f1 text COMPRESSION zstd_dict);
\d+ cmdict

-- before a dictionary is trained, zstd_dict compresses with plain zstd
INSERT INTO cmdict SELECT g, doc(g) FROM generate_series(1, 200) g;
SELECT pg_column_compression(f1), count(*) FROM cmdict GROUP BY 1;

-- only the owner can train; only for a zstd_dict column
CREATE ROLE regress_dict_user;
GRANT USAGE ON SCHEMA zstd_dict TO regress_dict_user;
GRANT SELECT ON cmdict TO regress_dict_user;
SET ROLE regress_dict_user;
SELECT pg_train_toast_dictionary('zstd_dict.cmdict', 'f1');
RESET ROLE;
CREATE TABLE cmplain (f1 text COMPRESSION zstd);
SELECT pg_train_toast_dictionary('cmplain', 'f1');
SELECT pg_train_toast_dictionary('cmdict', 'nosuchcol');
SELECT pg_train_toast_dictionary('cmdict', 'f1', 10);
-- the catalog is not readable by ordinary users
SET ROLE regress_dict_user;
SELECT count(*) FROM pg_toast_dictionary;
RESET ROLE;

SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;
SELECT count(*) AS ndicts, count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;

-- new values use the dictionary, and read back correctly, whole and sliced
INSERT INTO cmdict SELECT g, doc(g) FROM generate_series(201, 400) g;
SELECT pg_column_compression(f1), count(*) FROM cmdict GROUP BY 1 ORDER BY 1;
SELECT bool_and(f1 = doc(id)) AS all_equal FROM cmdict;
SELECT bool_and(substr(f1, 10, 30) = substr(doc(id), 10, 30)) AS slices_ok
  FROM cmdict WHERE id > 200;
-- the dictionary pays off: same data, smaller than without it
SELECT avg(pg_column_size(f1)) FILTER (WHERE pg_column_compression(f1) = 'zstd_dict') <
       avg(pg_column_size(f1)) FILTER (WHERE pg_column_compression(f1) = 'zstd')
       AS dict_smaller FROM cmdict;

-- large values are compressed with the dictionary and stored out of line
INSERT INTO cmdict SELECT 1000 + g / 100, string_agg(doc(g), ',')
  FROM generate_series(1, 299) g GROUP BY g / 100;
SELECT pg_column_compression(f1), pg_column_toast_chunk_id(f1) IS NOT NULL AS external
  FROM cmdict WHERE id > 1000 LIMIT 1;

----------------------------------------------------------------------
-- Containment: dictionary-compressed values do not leave their column
----------------------------------------------------------------------

-- CTAS and INSERT ... SELECT into another table: the target gets values that
-- are not compressed with cmdict's dictionary
CREATE TABLE cmcopy AS SELECT * FROM cmdict;
SELECT count(*) FILTER (WHERE pg_column_compression(f1) = 'zstd_dict') AS dict_values
  FROM cmcopy;
CREATE TABLE cmcopy2 (id int, f1 text COMPRESSION zstd_dict);
INSERT INTO cmcopy2 SELECT * FROM cmdict;
SELECT count(*) FILTER (WHERE pg_column_compression(f1) = 'zstd_dict') AS dict_values
  FROM cmcopy2;
-- another column of the same table does not own the dictionary either
ALTER TABLE cmdict ADD COLUMN f2 text COMPRESSION zstd_dict;
UPDATE cmdict SET f2 = f1 WHERE id <= 400;
SELECT count(*) FILTER (WHERE pg_column_compression(f2) = 'zstd_dict') AS dict_values
  FROM cmdict;
ALTER TABLE cmdict DROP COLUMN f2;

-- values nested in containers: rows, arrays, ranges of text are flattened
CREATE TYPE cmpair AS (a text, b text);
CREATE TABLE cmnest (p cmpair, arr text[]);
INSERT INTO cmnest SELECT ROW(f1, f1)::cmpair, ARRAY[f1, f1]
  FROM cmdict WHERE id BETWEEN 201 AND 210;
SELECT pg_column_compression((p).a) AS in_row, pg_column_compression(arr[1]) AS in_array
  FROM cmnest LIMIT 1;

-- a holdable cursor outlives the transaction; its rows must not depend on
-- the dictionary
BEGIN;
DECLARE c CURSOR WITH HOLD FOR SELECT id, f1 FROM cmdict WHERE id BETWEEN 201 AND 205 ORDER BY id;
COMMIT;
FETCH 1 FROM c;
CLOSE c;

-- PL/pgSQL variable across COMMIT in a procedure
CREATE PROCEDURE cmproc() LANGUAGE plpgsql AS $$
DECLARE v text;
BEGIN
  SELECT f1 INTO v FROM cmdict WHERE id = 300;
  COMMIT;
  RAISE NOTICE 'length %, ok %', length(v), v = doc(300);
END $$;
CALL cmproc();

-- values written to the table itself are unaffected by any of this
SELECT bool_and(f1 = doc(id)) AS all_equal FROM cmdict WHERE id <= 400;

-- indexes: an expression index on a dictionary-compressed column works
CREATE INDEX cmdict_idx ON cmdict (left(f1, 40));
CREATE INDEX cmdict_idx2 ON cmdict (f1) WHERE id = 300;
SELECT id FROM cmdict WHERE f1 = doc(300);

----------------------------------------------------------------------
-- Retirement
----------------------------------------------------------------------

-- retraining makes a new current dictionary; the old one stays readable
SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;
SELECT count(*) AS ndicts, count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT bool_and(f1 = doc(id)) AS all_equal FROM cmdict WHERE id <= 400;

-- ordinary updates of other columns leave the value, and its old dictionary,
-- alone; updates of the column recompress with the current one
UPDATE cmdict SET id = id WHERE id = 250;
UPDATE cmdict SET f1 = f1 || '' WHERE id = 251;

-- a rewrite recompresses with the current dictionary and retires the rest
VACUUM FULL cmdict;
SELECT count(*) AS ndicts, count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT bool_and(f1 = doc(id)) AS all_equal FROM cmdict WHERE id <= 400;
SELECT pg_column_compression(f1), count(*) FROM cmdict WHERE id <= 400 GROUP BY 1 ORDER BY 1;

-- the same for CLUSTER and a rewriting ALTER TABLE
SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;
CLUSTER cmdict USING cmdict_idx;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;
ALTER TABLE cmdict ALTER COLUMN id TYPE bigint;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT bool_and(f1 = doc(id::int)) AS all_equal FROM cmdict WHERE id <= 400;

-- switching the column away from zstd_dict: existing values stay readable
-- until the next rewrite, which recompresses them and drops the dictionary
ALTER TABLE cmdict ALTER COLUMN f1 SET COMPRESSION lz4;
SELECT bool_and(f1 = doc(id::int)) AS all_equal FROM cmdict WHERE id <= 400;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
VACUUM FULL cmdict;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT pg_column_compression(f1), count(*) FROM cmdict WHERE id <= 400 GROUP BY 1 ORDER BY 1;
SELECT bool_and(f1 = doc(id::int)) AS all_equal FROM cmdict WHERE id <= 400;
ALTER TABLE cmdict ALTER COLUMN f1 SET COMPRESSION zstd_dict;
SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;

----------------------------------------------------------------------
-- Cleanup with the owning objects
----------------------------------------------------------------------

ALTER TABLE cmdict ADD COLUMN f3 text COMPRESSION zstd_dict;
UPDATE cmdict SET f3 = f1 WHERE id <= 400;
SELECT pg_train_toast_dictionary('cmdict', 'f3', 4096) IS NOT NULL AS trained;
SELECT tdattnum > 2 AS f3_dict, count(*) FROM pg_toast_dictionary
  WHERE tdrelid = 'cmdict'::regclass GROUP BY 1 ORDER BY 1;
-- DROP COLUMN leaves the values on disk until a rewrite, and whole-row
-- references and REPLICA IDENTITY FULL still read them, so the dictionary
-- stays until then
ALTER TABLE cmdict DROP COLUMN f3;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;
SELECT count(*) AS wholerow FROM (SELECT t FROM cmdict t) s;
VACUUM FULL cmdict;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;

-- two trainings in one command: only the last one stays current
SELECT count(*) FROM (SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096)
  FROM generate_series(1, 2)) s;
SELECT count(*) AS ndicts, count(*) FILTER (WHERE tdcurrent) AS ncurrent
  FROM pg_toast_dictionary WHERE tdrelid = 'cmdict'::regclass;

-- the copies made above survive their source being dropped
SELECT 'cmdict'::regclass::oid AS cmdict_oid \gset
DROP TABLE cmdict;
SELECT count(*) AS ndicts FROM pg_toast_dictionary WHERE tdrelid = :cmdict_oid;
SELECT bool_and(f1 = doc(id)) AS copies_ok FROM cmcopy WHERE id <= 400;
SELECT bool_and(f1 = doc(id)) AS copies_ok FROM cmcopy2 WHERE id <= 400;
SELECT bool_and((p).a = arr[1]) AS nested_ok FROM cmnest;

\set HIDE_TOAST_COMPRESSION true
RESET search_path;
DROP SCHEMA zstd_dict CASCADE;
DROP ROLE regress_dict_user;
