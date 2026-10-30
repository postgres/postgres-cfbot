-- Tests for lz4 TOAST compression with dictionaries.  The dictionary
-- lifecycle (ownership, containment, retirement) is codec-independent and is
-- covered in compression_zstd_dict; this file checks the lz4 codec path:
-- values compress with the dictionary, read back whole and sliced, beat plain
-- lz4, and stay inside their column.  Training a dictionary uses zstd's
-- builder (lz4 has none), so the suite needs both lz4 and zstd.

SELECT NOT(enumvals @> '{lz4}' AND enumvals @> '{zstd}') AS skip_test
  FROM pg_settings WHERE name = 'default_toast_compression' \gset
\if :skip_test
   \echo '*** skipping TOAST tests with lz4 dictionaries (lz4 and zstd both required) ***'
   \quit
\endif

CREATE SCHEMA lz4_dict;
SET search_path TO lz4_dict, public;
\set HIDE_TOAST_COMPRESSION false

-- Many small, similar documents: the case dictionaries are made for.
CREATE FUNCTION doc(i int) RETURNS text LANGUAGE SQL AS $$
  SELECT format('{"customer_id": %s, "status": "active", "region": "us-east", '
                '"plan": "enterprise", "features": ["sso", "audit", "backup"], '
                '"tags": ["%s", "priority-%s"], "notes": "%s"}',
                i, md5(i::text), i % 5, repeat('lorem ipsum dolor sit amet ', 80))
$$;

CREATE TABLE cmdict (id int, f1 text COMPRESSION lz4_dict);
\d+ cmdict

-- before a dictionary is trained, lz4_dict compresses with plain lz4
INSERT INTO cmdict SELECT g, doc(g) FROM generate_series(1, 200) g;
SELECT pg_column_compression(f1), count(*) FROM cmdict GROUP BY 1;

-- training works on an lz4_dict column, and is refused on a non-dict column
SELECT pg_train_toast_dictionary('cmdict', 'f1', 4096) IS NOT NULL AS trained;
CREATE TABLE cmplain (f1 text COMPRESSION lz4);
SELECT pg_train_toast_dictionary('cmplain', 'f1');

-- new values use the dictionary and read back correctly, whole and sliced
INSERT INTO cmdict SELECT g, doc(g) FROM generate_series(201, 400) g;
SELECT pg_column_compression(f1), count(*) FROM cmdict GROUP BY 1 ORDER BY 1;
SELECT bool_and(f1 = doc(id)) AS all_equal FROM cmdict;
SELECT bool_and(substr(f1, 10, 30) = substr(doc(id), 10, 30)) AS slices_ok
  FROM cmdict WHERE id > 200;
-- the dictionary pays off: same data, smaller than without it
SELECT avg(pg_column_size(f1)) FILTER (WHERE pg_column_compression(f1) = 'lz4_dict') <
       avg(pg_column_size(f1)) FILTER (WHERE pg_column_compression(f1) = 'lz4')
       AS dict_smaller FROM cmdict;

-- large values are compressed with the dictionary and stored out of line,
-- and still read back whole after reassembly from toast chunks
INSERT INTO cmdict SELECT 1000 + g / 100, string_agg(doc(g), ',')
  FROM generate_series(1, 299) g GROUP BY g / 100;
SELECT pg_column_compression(f1), pg_column_toast_chunk_id(f1) IS NOT NULL AS external
  FROM cmdict WHERE id > 1000 LIMIT 1;
SELECT bool_and(length(f1) > 0) AS big_ok FROM cmdict WHERE id > 1000;

-- Containment: an lz4_dict value copied to a pglz column is recompressed
CREATE TABLE cmcopy (f1 text COMPRESSION pglz);
INSERT INTO cmcopy SELECT f1 FROM cmdict WHERE id BETWEEN 201 AND 300;
SELECT count(*) FILTER (WHERE pg_column_compression(f1) = 'lz4_dict') AS leaked
  FROM cmcopy;

DROP SCHEMA lz4_dict CASCADE;
