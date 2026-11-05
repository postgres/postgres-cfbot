/* contrib/amcheck/amcheck--1.5--1.6.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION amcheck UPDATE TO '1.6'" to load this file. \quit


-- gist_index_check()
--
CREATE FUNCTION gist_index_check(index regclass, heapallindexed boolean)
RETURNS VOID
AS 'MODULE_PATHNAME', 'gist_index_check'
LANGUAGE C STRICT;

REVOKE ALL ON FUNCTION gist_index_check(regclass, boolean) FROM PUBLIC;
