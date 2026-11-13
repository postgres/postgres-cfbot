/* contrib/pg_walinspect/pg_walinspect--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_walinspect UPDATE TO '1.2'" to load this file. \quit

--
-- pg_get_wal_location_at_time()
--
CREATE FUNCTION pg_get_wal_location_at_time(
    IN target_time timestamptz,
    IN before interval DEFAULT '1 minute',
    IN after interval DEFAULT '1 minute',
    OUT start_timestamp timestamptz,
    OUT start_lsn pg_lsn,
    OUT end_timestamp timestamptz,
    OUT end_lsn pg_lsn
)
AS 'MODULE_PATHNAME', 'pg_get_wal_location_at_time'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION
  pg_get_wal_location_at_time(timestamptz, interval, interval) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION
  pg_get_wal_location_at_time(timestamptz, interval, interval)
  TO pg_read_server_files;

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
