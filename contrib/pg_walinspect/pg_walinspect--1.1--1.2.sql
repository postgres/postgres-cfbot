/* contrib/pg_walinspect/pg_walinspect--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_walinspect UPDATE TO '1.2'" to load this file. \quit

--
-- pg_get_wal_files()
--
CREATE FUNCTION pg_get_wal_files(
    IN start_lsn pg_lsn,
    IN end_lsn pg_lsn DEFAULT NULL,
    OUT wal_file text,
    OUT segment_start_lsn pg_lsn,
    OUT segment_end_lsn pg_lsn
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_files'
LANGUAGE C PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_files(pg_lsn, pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_files(pg_lsn, pg_lsn)
  TO pg_read_server_files;
