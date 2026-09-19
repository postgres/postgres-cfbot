/* src/test/modules/test_deletemark/test_deletemark--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_deletemark" to load this file. \quit

CREATE FUNCTION dm_classify(idx regclass, t tid)
RETURNS text STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION dm_heaptid(idx regclass, t tid)
RETURNS tid STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION dm_mark(idx regclass, t tid)
RETURNS boolean STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION dm_scan_recheck(idx regclass)
RETURNS boolean STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;
