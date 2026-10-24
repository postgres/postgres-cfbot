/* contrib/pg_walinspect/pg_walinspect--1.1--1.2.sql */

\echo Use "ALTER EXTENSION pg_walinspect UPDATE TO '1.2'" to load this file. \quit

-- Append physical sizes without changing the existing, decoded-size columns.
DROP FUNCTION pg_get_wal_record_info(pg_lsn);
CREATE FUNCTION pg_get_wal_record_info(IN in_lsn pg_lsn,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT fpi_length int4,
    OUT description text,
    OUT block_ref text,
    OUT physical_length int4
)
AS 'MODULE_PATHNAME', 'pg_get_wal_record_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn) TO pg_read_server_files;

DROP FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn);
CREATE FUNCTION pg_get_wal_records_info(IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT fpi_length int4,
    OUT description text,
    OUT block_ref text,
    OUT physical_length int4
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_records_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn) TO pg_read_server_files;

DROP FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean);
CREATE FUNCTION pg_get_wal_block_info(IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    IN show_data boolean DEFAULT true,
    OUT start_lsn pg_lsn,
    OUT end_lsn pg_lsn,
    OUT prev_lsn pg_lsn,
    OUT block_id int2,
    OUT reltablespace oid,
    OUT reldatabase oid,
    OUT relfilenode oid,
    OUT relforknumber int2,
    OUT relblocknumber int8,
    OUT xid xid,
    OUT resource_manager text,
    OUT record_type text,
    OUT record_length int4,
    OUT main_data_length int4,
    OUT block_data_length int4,
    OUT block_fpi_length int4,
    OUT block_fpi_info text[],
    OUT description text,
    OUT block_data bytea,
    OUT block_fpi_data bytea,
    OUT physical_length int4
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_block_info'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) TO pg_read_server_files;

DROP FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean);
CREATE FUNCTION pg_get_wal_stats(IN start_lsn pg_lsn,
    IN end_lsn pg_lsn,
    IN per_record boolean DEFAULT false,
    OUT "resource_manager/record_type" text,
    OUT count int8,
    OUT count_percentage float8,
    OUT record_size int8,
    OUT record_size_percentage float8,
    OUT fpi_size int8,
    OUT fpi_size_percentage float8,
    OUT combined_size int8,
    OUT combined_size_percentage float8,
    OUT physical_size int8
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_get_wal_stats'
LANGUAGE C STRICT PARALLEL SAFE;

REVOKE EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean) FROM PUBLIC;
GRANT EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean) TO pg_read_server_files;
