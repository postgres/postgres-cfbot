/* src/test/modules/test_tidbitmap/test_tidbitmap--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_tidbitmap" to load this file. \quit

CREATE FUNCTION test_tidbitmap_check(nbitmaps int, npages int, maxkb int,
									 lossy_every int, error_at int)
RETURNS bigint STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_tidbitmap_setop(op text, n int)
RETURNS bigint STRICT PARALLEL UNSAFE
AS 'MODULE_PATHNAME' LANGUAGE C;
