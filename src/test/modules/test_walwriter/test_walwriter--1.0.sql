/* src/test/modules/test_walwriter/test_walwriter--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_walwriter" to load this file. \quit

CREATE FUNCTION test_walwriter_bogus_async_lsn()
RETURNS pg_lsn
AS 'MODULE_PATHNAME' LANGUAGE C;
