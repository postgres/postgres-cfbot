/* contrib/pageinspect/pageinspect--1.13--1.14.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pageinspect UPDATE TO '1.14'" to load this file. \quit

--
-- gin_entrypage_items()
--
CREATE FUNCTION gin_entrypage_items(IN page bytea, IN reloid OID,
    OUT itemoffset smallint,
    OUT downlink tid,
    OUT tids tid[],
    OUT keys text)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gin_entrypage_items'
LANGUAGE C STRICT PARALLEL SAFE;

--
-- gin_datapage_items()
--
CREATE FUNCTION gin_datapage_items(IN page bytea,
    OUT itemoffset smallint,
    OUT downlink int,
    OUT item_tid tid)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'gin_datapage_items'
LANGUAGE C STRICT PARALLEL SAFE;
