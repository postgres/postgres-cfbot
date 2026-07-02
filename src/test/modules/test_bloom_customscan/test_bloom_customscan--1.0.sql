/* src/test/modules/test_bloom_customscan/test_bloom_customscan--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_bloom_customscan" to load this file. \quit

CREATE FUNCTION test_bloom_cs_reset()
	RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bloom_cs_scanned_rows()
	RETURNS bigint
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bloom_cs_rejected_rows()
	RETURNS bigint
	AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_bloom_cs_perkey_built()
	RETURNS boolean
	AS 'MODULE_PATHNAME' LANGUAGE C;
