-- Tests for TOAST compression with zstd

SELECT NOT(enumvals @> '{zstd}') AS skip_test FROM pg_settings WHERE
  name = 'default_toast_compression' \gset
\if :skip_test
   \echo '*** skipping TOAST tests with zstd (not supported) ***'
   \quit
\endif

CREATE SCHEMA zstd;
SET search_path TO zstd, public;

\set HIDE_TOAST_COMPRESSION false

-- Ensure we get stable results regardless of the installation's default.
-- We rely on this GUC value for a few tests.
SET default_toast_compression = 'pglz';

-- Helpers.  Hash output is poorly compressible, so values built from it
-- stay large after compression and are stored out of line; a bytea of raw
-- hash bytes is not compressible at all.  None of the checks below print
-- compressed sizes, which depend on the zstd version.
CREATE FUNCTION large_val_zstd() RETURNS TEXT LANGUAGE SQL AS
'select array_agg(fipshash(g::text))::text from generate_series(1, 256) g';
CREATE FUNCTION incompressible_val() RETURNS bytea LANGUAGE SQL AS
$$ select decode(string_agg(fipshash(g::text), '' order by g), 'hex')
   from generate_series(1, 200) g $$;

----------------------------------------------------------------------
-- DDL
----------------------------------------------------------------------

-- test creating table with compression method
CREATE TABLE cmdata_pglz(f1 text COMPRESSION pglz);
CREATE INDEX idx ON cmdata_pglz(f1);
INSERT INTO cmdata_pglz VALUES(repeat('1234567890', 1000));
\d+ cmdata_pglz
CREATE TABLE cmdata_zstd(f1 TEXT COMPRESSION zstd);
INSERT INTO cmdata_zstd VALUES(repeat('1234567890', 1004));
\d+ cmdata_zstd

-- the method name is an identifier, so case does not matter
CREATE TABLE cmupper(f1 text COMPRESSION ZSTD);
\d+ cmupper
DROP TABLE cmupper;

-- a domain over a varlena type can be compressed too
CREATE DOMAIN zstdtext AS text;
CREATE TABLE cmdomain(f1 zstdtext COMPRESSION zstd);
INSERT INTO cmdomain VALUES (repeat('1234567890', 1004));
\d+ cmdomain
SELECT pg_column_compression(f1), length(f1) FROM cmdomain;
DROP TABLE cmdomain;
DROP DOMAIN zstdtext;

-- errors: incompressible data type, unknown method
CREATE TABLE cmbad(f1 int COMPRESSION zstd);
CREATE TABLE cmbad(f1 text);
ALTER TABLE cmbad ALTER COLUMN f1 SET COMPRESSION zstd_nodict;
DROP TABLE cmbad;

----------------------------------------------------------------------
-- Compressed-in-line values
----------------------------------------------------------------------

-- verify stored compression method in the data, and that compression
-- did save space
SELECT pg_column_compression(f1),
       pg_column_size(f1) < length(f1) AS smaller,
       pg_column_toast_chunk_id(f1) IS NULL AS is_inline
  FROM cmdata_zstd;

-- decompress data slice
SELECT SUBSTR(f1, 200, 5) FROM cmdata_pglz;
SELECT SUBSTR(f1, 2000, 50) FROM cmdata_zstd;

-- check that slice decompression agrees with full decompression, at the
-- start, in the middle and at the very end of the value
SELECT SUBSTR(f1, 1, 100) = SUBSTR(repeat('1234567890', 1004), 1, 100) AS ok_head,
       SUBSTR(f1, 5000, 100) = SUBSTR(repeat('1234567890', 1004), 5000, 100) AS ok_mid,
       SUBSTR(f1, 10036, 5) = SUBSTR(repeat('1234567890', 1004), 10036, 5) AS ok_tail,
       length(f1) AS len
  FROM cmdata_zstd;

-- slices of one byte, past the end, and longer than the whole value (the
-- latter falls back to a full decompression)
SELECT left(f1, 1) AS first_char,
       right(f1, 1) AS last_char,
       SUBSTR(f1, 10040, 1) AS at_end,
       SUBSTR(f1, 10041, 10) = '' AS past_end,
       length(SUBSTR(f1, 1, 1000000)) AS full_len
  FROM cmdata_zstd;

----------------------------------------------------------------------
-- Values that do not get compressed
----------------------------------------------------------------------

-- values below the TOAST threshold are stored as they are; NULL and the
-- empty string are fine too
CREATE TABLE cmshort(f1 text COMPRESSION zstd);
INSERT INTO cmshort VALUES (repeat('x', 100)), (''), (NULL);
SELECT pg_column_compression(f1), length(f1) FROM cmshort ORDER BY length(f1);
DROP TABLE cmshort;

-- data that zstd cannot shrink is stored uncompressed, out of line if
-- needed, with no compression method recorded
CREATE TABLE cmincompressible(f1 bytea COMPRESSION zstd);
INSERT INTO cmincompressible SELECT incompressible_val();
SELECT pg_column_compression(f1) IS NULL AS uncompressed,
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_size(f1) = octet_length(f1) AS same_size,
       length(f1) AS len,
       f1 = incompressible_val() AS ok_full,
       substr(f1, 3000, 40) = substr(incompressible_val(), 3000, 40) AS ok_slice
  FROM cmincompressible;
DROP TABLE cmincompressible;

----------------------------------------------------------------------
-- Other varlena types
----------------------------------------------------------------------

-- binary data with zero and high bytes
CREATE TABLE cmbytea(f1 bytea COMPRESSION zstd);
INSERT INTO cmbytea VALUES (decode(repeat('00ff10', 2000), 'hex'));
SELECT pg_column_compression(f1), length(f1),
       f1 = decode(repeat('00ff10', 2000), 'hex') AS ok_full,
       get_byte(f1, 0) AS byte0, get_byte(f1, 1) AS byte1,
       substr(f1, 3001, 3) = decode('00ff10', 'hex') AS ok_slice
  FROM cmbytea;
DROP TABLE cmbytea;

-- jsonb, compressed inline and out of line
CREATE TABLE cmjsonb(j jsonb COMPRESSION zstd);
INSERT INTO cmjsonb SELECT jsonb_build_object('k', repeat('v', 5000), 'n', 1);
INSERT INTO cmjsonb SELECT jsonb_object_agg(g::text, fipshash(g::text))
  FROM generate_series(1, 300) g;
SELECT pg_column_compression(j),
       pg_column_toast_chunk_id(j) IS NOT NULL AS is_external,
       length(j ->> 'k') AS klen,
       j ->> 'n' AS n,
       j ->> '150' = fipshash('150') AS ok_150
  FROM cmjsonb ORDER BY is_external;
DROP TABLE cmjsonb;

-- arrays
CREATE TABLE cmarray(f1 text[] COMPRESSION zstd);
INSERT INTO cmarray SELECT array_agg(repeat('x', 100) || g ORDER BY g)
  FROM generate_series(1, 200) g;
SELECT pg_column_compression(f1), array_length(f1, 1) AS n,
       f1[1] = repeat('x', 100) || '1' AS ok_first,
       f1[200] = repeat('x', 100) || '200' AS ok_last
  FROM cmarray;
DROP TABLE cmarray;

----------------------------------------------------------------------
-- Moving data between tables and methods
----------------------------------------------------------------------

-- copy with table creation
SELECT * INTO cmmove1 FROM cmdata_zstd;
\d+ cmmove1
SELECT pg_column_compression(f1) FROM cmmove1;

-- test LIKE INCLUDING COMPRESSION.  The GUC default_toast_compression
-- has no effect, the compression method is taken from the table being copied.
CREATE TABLE cmdata2 (LIKE cmdata_zstd INCLUDING COMPRESSION);
\d+ cmdata2
DROP TABLE cmdata2;

-- copy to existing table: a compressed datum keeps its own method, in both
-- directions, so a table can hold values compressed with different methods
CREATE TABLE cmmove3(f1 text COMPRESSION pglz);
INSERT INTO cmmove3 SELECT * FROM cmdata_pglz;
INSERT INTO cmmove3 SELECT * FROM cmdata_zstd;
SELECT pg_column_compression(f1) FROM cmmove3 ORDER BY 1;
INSERT INTO cmdata_zstd SELECT * FROM cmdata_pglz;
SELECT pg_column_compression(f1), count(*) FROM cmdata_zstd GROUP BY 1 ORDER BY 1;
DELETE FROM cmdata_zstd WHERE pg_column_compression(f1) = 'pglz';

-- update using datum from different table with zstd data.
CREATE TABLE cmmove2(f1 text COMPRESSION pglz);
INSERT INTO cmmove2 VALUES (repeat('1234567890', 1004));
SELECT pg_column_compression(f1) FROM cmmove2;
UPDATE cmmove2 SET f1 = cmdata_zstd.f1 FROM cmdata_zstd;
SELECT pg_column_compression(f1) FROM cmmove2;

----------------------------------------------------------------------
-- Externally stored compressed values
----------------------------------------------------------------------

CREATE TABLE cmdata2 (f1 text COMPRESSION zstd);
INSERT INTO cmdata2 SELECT large_val_zstd() || repeat('a', 4000);
SELECT pg_column_compression(f1), pg_column_size(f1) < length(f1) AS smaller
  FROM cmdata2;
SELECT SUBSTR(f1, 200, 5) FROM cmdata2;
SELECT SUBSTR(f1, 200, 5) = SUBSTR(large_val_zstd() || repeat('a', 4000), 200, 5) AS ok_slice
  FROM cmdata2;
DROP TABLE cmdata2;

-- test that both TOAST value ID widths carry the compression method through
-- an external TOAST pointer.  The data here is poorly compressible on
-- purpose, so that the value is certain to be stored out of line.
CREATE TABLE cmdata_oid(f1 text COMPRESSION zstd)
  WITH (toast_value_type = 'oid');
CREATE TABLE cmdata_oid8(f1 text COMPRESSION zstd)
  WITH (toast_value_type = 'oid8');
INSERT INTO cmdata_oid SELECT large_val_zstd();
INSERT INTO cmdata_oid8 SELECT large_val_zstd();
-- confirm the values really are out of line, so that the checks below
-- exercise the external TOAST pointer rather than the inline header
SELECT pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_compression(f1) FROM cmdata_oid;
SELECT pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_compression(f1) FROM cmdata_oid8;
SELECT length(f1) = length(large_val_zstd()) AS ok_len,
       f1 = large_val_zstd() AS ok_full,
       SUBSTR(f1, 3000, 40) = SUBSTR(large_val_zstd(), 3000, 40) AS ok_slice
  FROM cmdata_oid;
SELECT length(f1) = length(large_val_zstd()) AS ok_len,
       f1 = large_val_zstd() AS ok_full,
       SUBSTR(f1, 3000, 40) = SUBSTR(large_val_zstd(), 3000, 40) AS ok_slice
  FROM cmdata_oid8;

-- a value spanning many TOAST chunks, with slices from the front, from
-- near the end (which has to decompress almost everything), and longer
-- than the value
CREATE TABLE cmbig(f1 text COMPRESSION zstd);
INSERT INTO cmbig SELECT string_agg(fipshash(g::text) || repeat('-', 40), '' ORDER BY g)
  FROM generate_series(1, 10000) g;
SELECT pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_size(f1) < length(f1) AS smaller,
       length(f1) AS len,
       left(f1, 32) = fipshash('1') AS ok_head,
       SUBSTR(f1, 360001, 32) = fipshash('5001') AS ok_mid,
       SUBSTR(f1, 719929, 32) = fipshash('10000') AS ok_tail,
       length(SUBSTR(f1, 1, 2000000)) AS full_len
  FROM cmbig;
DROP TABLE cmbig;

-- a smaller toast_tuple_target makes even a small compressed value go out
-- of line
CREATE TABLE cmtarget(f1 text COMPRESSION zstd) WITH (toast_tuple_target = 128);
INSERT INTO cmtarget SELECT substr(large_val_zstd(), 1, 3000);
SELECT pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       f1 = substr(large_val_zstd(), 1, 3000) AS ok_full
  FROM cmtarget;
DROP TABLE cmtarget;

-- an external zstd value copied into a table with a different chunk_id type
-- and compression setting keeps its compression, and is re-externalized
-- with a fresh TOAST pointer; try both chunk_id types as destination
CREATE TABLE cmmove_ext (f1 text COMPRESSION pglz) WITH (toast_value_type = 'oid');
CREATE TABLE cmmove_ext8 (f1 text COMPRESSION pglz) WITH (toast_value_type = 'oid8');
INSERT INTO cmmove_ext SELECT f1 FROM cmdata_oid8;
INSERT INTO cmmove_ext8 SELECT f1 FROM cmdata_oid;
SELECT pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_compression(f1),
       f1 = large_val_zstd() AS ok_full
  FROM cmmove_ext;
SELECT pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_compression(f1),
       f1 = large_val_zstd() AS ok_full
  FROM cmmove_ext8;
DROP TABLE cmmove_ext, cmmove_ext8;

----------------------------------------------------------------------
-- Storage modes
----------------------------------------------------------------------

-- EXTERNAL: stored out of line but never compressed, slices cross chunk
-- boundaries directly
CREATE TABLE cmstorage(f1 text COMPRESSION zstd);
ALTER TABLE cmstorage ALTER COLUMN f1 SET STORAGE EXTERNAL;
INSERT INTO cmstorage SELECT large_val_zstd();
SELECT pg_column_compression(f1) IS NULL AS uncompressed,
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       pg_column_size(f1) = octet_length(f1) AS same_size,
       SUBSTR(f1, 1990, 20) = SUBSTR(large_val_zstd(), 1990, 20) AS ok_slice
  FROM cmstorage;

-- MAIN: compressed, and kept in the tuple when that makes it fit
TRUNCATE cmstorage;
ALTER TABLE cmstorage ALTER COLUMN f1 SET STORAGE MAIN;
INSERT INTO cmstorage VALUES (repeat('1234567890', 1004));
SELECT pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NULL AS is_inline,
       length(f1) AS len
  FROM cmstorage;
DROP TABLE cmstorage;

-- PLAIN: new values are never compressed.  An external compressed datum
-- coming from another table is fetched and decompressed before being
-- stored, while a datum that is already compressed in line is stored as it
-- is, like for the other methods.
CREATE TABLE cmsmall(f1 text COMPRESSION zstd) WITH (toast_tuple_target = 128);
INSERT INTO cmsmall VALUES (repeat('1234567890', 300));
INSERT INTO cmsmall SELECT substr(large_val_zstd(), 1, 3000);
SELECT pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external
  FROM cmsmall ORDER BY is_external;
CREATE TABLE cmplain(f1 text COMPRESSION zstd);
ALTER TABLE cmplain ALTER COLUMN f1 SET STORAGE PLAIN;
INSERT INTO cmplain VALUES (repeat('1234567890', 300));
INSERT INTO cmplain SELECT f1 FROM cmsmall;
SELECT pg_column_compression(f1), length(f1),
       f1 IN (repeat('1234567890', 300), substr(large_val_zstd(), 1, 3000)) AS ok_full
  FROM cmplain ORDER BY pg_column_compression(f1) NULLS FIRST, length(f1);
DROP TABLE cmplain, cmsmall;

----------------------------------------------------------------------
-- Table rewrites and updates of external values
----------------------------------------------------------------------

CREATE TABLE cmrewrite(id int, f1 text COMPRESSION zstd)
  WITH (toast_value_type = 'oid8');
INSERT INTO cmrewrite VALUES (1, large_val_zstd()), (2, repeat('1234567890', 1004));
CREATE INDEX cmrewrite_id ON cmrewrite(id);

-- VACUUM FULL and CLUSTER preserve the values and do not recompress
VACUUM FULL cmrewrite;
SELECT id, pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       f1 = CASE id WHEN 1 THEN large_val_zstd() ELSE repeat('1234567890', 1004) END AS ok_full
  FROM cmrewrite ORDER BY id;
CLUSTER cmrewrite USING cmrewrite_id;
SELECT id, pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       f1 = CASE id WHEN 1 THEN large_val_zstd() ELSE repeat('1234567890', 1004) END AS ok_full
  FROM cmrewrite ORDER BY id;

-- a column type change resets the column's compression method to the
-- default (as it does its storage), so the rewritten values follow the
-- default_toast_compression GUC; values that were external come back in
-- one piece
ALTER TABLE cmrewrite ALTER COLUMN f1 TYPE varchar(30000);
\d+ cmrewrite
SELECT id, pg_column_compression(f1),
       f1 = CASE id WHEN 1 THEN large_val_zstd() ELSE repeat('1234567890', 1004) END AS ok_full
  FROM cmrewrite ORDER BY id;
SET default_toast_compression = 'zstd';
ALTER TABLE cmrewrite ALTER COLUMN f1 TYPE varchar(20000);
SELECT id, pg_column_compression(f1),
       f1 = CASE id WHEN 1 THEN large_val_zstd() ELSE repeat('1234567890', 1004) END AS ok_full
  FROM cmrewrite ORDER BY id;
SET default_toast_compression = 'pglz';
DROP TABLE cmrewrite;

-- an UPDATE that leaves the external value alone must keep the same TOAST
-- value rather than re-toasting it
ALTER TABLE cmdata_oid ADD COLUMN n int DEFAULT 0;
SELECT pg_column_toast_chunk_id(f1) AS chunk_before FROM cmdata_oid \gset
UPDATE cmdata_oid SET n = 1;
SELECT pg_column_toast_chunk_id(f1) = :'chunk_before' AS same_toast_value,
       f1 = large_val_zstd() AS ok_full
  FROM cmdata_oid;

-- whereas an UPDATE of the value itself replaces the TOAST value; the old
-- chunks are gone from the TOAST table as soon as the update commits
SELECT reltoastrelid::regclass AS toastrel FROM pg_class
  WHERE oid = 'cmdata_oid'::regclass \gset
UPDATE cmdata_oid SET f1 = f1 || 'x';
SELECT pg_column_toast_chunk_id(f1) <> :'chunk_before' AS new_toast_value,
       pg_column_compression(f1),
       f1 = large_val_zstd() || 'x' AS ok_full
  FROM cmdata_oid;
SELECT count(DISTINCT chunk_id) AS live_toast_values FROM :toastrel;

-- deleting the row deletes its TOAST value, for both chunk_id types
DELETE FROM cmdata_oid;
SELECT count(*) AS chunks_left FROM :toastrel;
SELECT reltoastrelid::regclass AS toastrel FROM pg_class
  WHERE oid = 'cmdata_oid8'::regclass \gset
DELETE FROM cmdata_oid8;
SELECT count(*) AS chunks_left FROM :toastrel;
DROP TABLE cmdata_oid, cmdata_oid8;

----------------------------------------------------------------------
-- Indexes
----------------------------------------------------------------------

-- a compressible key large enough to need compression to fit in an index
-- entry; the index must be usable to find the row
CREATE TABLE cmindex(f1 text COMPRESSION zstd);
CREATE INDEX cmindex_f1 ON cmindex(f1);
INSERT INTO cmindex VALUES (repeat('abcdefghij', 300)), (repeat('klmnopqrst', 300));
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT length(f1), left(f1, 10) FROM cmindex WHERE f1 = repeat('klmnopqrst', 300);
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE cmindex;

-- test expression index
CREATE TABLE cmdata2 (f1 TEXT COMPRESSION pglz, f2 TEXT COMPRESSION zstd);
CREATE UNIQUE INDEX idx1 ON cmdata2 ((f1 || f2));
INSERT INTO cmdata2 VALUES((SELECT array_agg(fipshash(g::TEXT))::TEXT FROM
generate_series(1, 50) g), VERSION());
DROP TABLE cmdata2;

----------------------------------------------------------------------
-- Materialized views, partitions, inheritance, temporary tables
----------------------------------------------------------------------

-- test compression with materialized view
CREATE MATERIALIZED VIEW compressmv(x) AS SELECT * FROM cmdata_zstd;
\d+ compressmv
SELECT pg_column_compression(f1) FROM cmdata_zstd;
SELECT pg_column_compression(x) FROM compressmv;

-- test compression with partition
CREATE TABLE cmpart(f1 text COMPRESSION zstd) PARTITION BY HASH(f1);
CREATE TABLE cmpart1 PARTITION OF cmpart FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE cmpart2(f1 text COMPRESSION pglz);

ALTER TABLE cmpart ATTACH PARTITION cmpart2 FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- test compression with inheritance
CREATE TABLE cminh() INHERITS(cmdata_pglz, cmdata_zstd); -- error
CREATE TABLE cminh(f1 TEXT COMPRESSION zstd) INHERITS(cmdata_pglz); -- error
CREATE TABLE cmdata3(f1 text);
CREATE TABLE cminh() INHERITS (cmdata_pglz, cmdata3);

-- temporary table
CREATE TEMP TABLE cmtemp(f1 text COMPRESSION zstd);
INSERT INTO cmtemp SELECT large_val_zstd();
SELECT pg_column_compression(f1),
       pg_column_toast_chunk_id(f1) IS NOT NULL AS is_external,
       f1 = large_val_zstd() AS ok_full
  FROM cmtemp;
DROP TABLE cmtemp;

----------------------------------------------------------------------
-- default_toast_compression GUC
----------------------------------------------------------------------

-- test default_toast_compression GUC; the value is case-insensitive
SET default_toast_compression = 'ZSTD';
SHOW default_toast_compression;
SET default_toast_compression = 'zstd';

-- the GUC drives the method for columns with no explicit COMPRESSION, so
-- the column keeps a default attcompression but the data comes out zstd
CREATE TABLE cmdata_default(f1 text);
INSERT INTO cmdata_default VALUES(repeat('1234567890', 1004));
\d+ cmdata_default
SELECT pg_column_compression(f1) FROM cmdata_default;
SELECT f1 = repeat('1234567890', 1004) AS ok_full,
       SUBSTR(f1, 4000, 20) = SUBSTR(repeat('1234567890', 1004), 4000, 20) AS ok_slice
  FROM cmdata_default;

-- SET COMPRESSION default makes a column follow the GUC again
ALTER TABLE cmdata_default ALTER COLUMN f1 SET COMPRESSION pglz;
INSERT INTO cmdata_default VALUES(repeat('1234567890', 1004));
ALTER TABLE cmdata_default ALTER COLUMN f1 SET COMPRESSION default;
\d+ cmdata_default
INSERT INTO cmdata_default VALUES(repeat('1234567890', 1004));
SELECT pg_column_compression(f1), count(*) FROM cmdata_default GROUP BY 1 ORDER BY 1;
DROP TABLE cmdata_default;

-- test alter compression method
ALTER TABLE cmdata_pglz ALTER COLUMN f1 SET COMPRESSION zstd;
INSERT INTO cmdata_pglz VALUES (repeat('123456789', 4004));
\d+ cmdata_pglz
SELECT pg_column_compression(f1) FROM cmdata_pglz;
ALTER TABLE cmdata_pglz ALTER COLUMN f1 SET COMPRESSION pglz;

-- test alter compression method for materialized views
ALTER MATERIALIZED VIEW compressmv ALTER COLUMN x SET COMPRESSION zstd;
\d+ compressmv

-- test alter compression method for partitioned tables
ALTER TABLE cmpart1 ALTER COLUMN f1 SET COMPRESSION pglz;
ALTER TABLE cmpart2 ALTER COLUMN f1 SET COMPRESSION zstd;

-- new data should be compressed with the current compression method
INSERT INTO cmpart VALUES (repeat('123456789', 1004));
INSERT INTO cmpart VALUES (repeat('123456789', 4004));
SELECT pg_column_compression(f1) FROM cmpart1;
SELECT pg_column_compression(f1) FROM cmpart2;

-- VACUUM FULL does not recompress
SELECT pg_column_compression(f1) FROM cmdata_zstd;
VACUUM FULL cmdata_zstd;
SELECT pg_column_compression(f1) FROM cmdata_zstd;

RESET default_toast_compression;

-- check data is ok
SELECT length(f1) FROM cmdata_pglz;
SELECT length(f1) FROM cmdata_zstd;
SELECT length(f1) FROM cmmove1;
SELECT length(f1) FROM cmmove2;
SELECT length(f1) FROM cmmove3;

\set HIDE_TOAST_COMPRESSION true
