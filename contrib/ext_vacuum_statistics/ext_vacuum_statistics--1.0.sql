/*-------------------------------------------------------------------------
 *
 * ext_vacuum_statistics--1.0.sql
 *    Extended vacuum statistics via hook and custom storage
 *
 * This extension collects extended vacuum statistics via set_report_vacuum_hook
 * and stores them in shared memory.
 *
 *-------------------------------------------------------------------------
 */

\echo Use "CREATE EXTENSION ext_vacuum_statistics" to load this file. \quit

CREATE SCHEMA IF NOT EXISTS ext_vacuum_statistics;

COMMENT ON SCHEMA ext_vacuum_statistics IS
  'Extended vacuum statistics (heap, index, database)';

-- Reset functions
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.extvac_reset_entry(
    dboid oid,
    relid oid
)
RETURNS boolean
AS 'MODULE_PATHNAME', 'extvac_reset_entry'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.extvac_reset_db_entry(dboid oid)
RETURNS bigint
AS 'MODULE_PATHNAME', 'extvac_reset_db_entry'
LANGUAGE C STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION ext_vacuum_statistics.vacuum_statistics_reset()
RETURNS bigint
AS 'MODULE_PATHNAME', 'vacuum_statistics_reset'
LANGUAGE C STRICT PARALLEL SAFE;

-- Internal C function to fetch table vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_tables(
    IN  dboid oid,
    IN  reloid oid,
    OUT relid oid,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric,
    OUT tuples_deleted bigint,
    OUT pages_scanned bigint,
    OUT pages_removed bigint,
    OUT tuples_frozen bigint,
    OUT recently_dead_tuples bigint,
    OUT missed_dead_pages bigint,
    OUT missed_dead_tuples bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_tables'
LANGUAGE C STRICT STABLE;

-- Internal C function to fetch index vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_indexes(
    IN  dboid oid,
    IN  reloid oid,
    OUT relid oid,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric,
    OUT tuples_deleted bigint,
    OUT pages_deleted bigint
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_indexes'
LANGUAGE C STRICT STABLE;

-- Internal C function to fetch database vacuum stats
CREATE OR REPLACE FUNCTION ext_vacuum_statistics.pg_stats_get_vacuum_database(
    IN  dboid oid,
    OUT dbid oid,
    OUT wal_records bigint,
    OUT wal_fpi bigint,
    OUT wal_bytes numeric
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pg_stats_get_vacuum_database'
LANGUAGE C STRICT STABLE;

-- View: vacuum statistics per table (heap)
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_tables AS
SELECT
  rel.oid AS relid,
  ns.nspname AS schema,
  rel.relname AS relname,
  db.datname AS dbname,
  stats.wal_records,
  stats.wal_fpi,
  stats.wal_bytes,
  stats.tuples_deleted,
  stats.pages_scanned,
  stats.pages_removed,
  stats.tuples_frozen,
  stats.recently_dead_tuples,
  stats.missed_dead_pages,
  stats.missed_dead_tuples
FROM pg_database db,
     pg_class rel,
     pg_namespace ns,
     LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_tables(db.oid, rel.oid) stats
WHERE db.datname = current_database()
  AND rel.relkind = 'r'
  AND rel.relnamespace = ns.oid
  AND rel.oid = stats.relid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_tables IS
  'Extended vacuum statistics per table (heap)';

-- View: vacuum statistics per index
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_indexes AS
SELECT
  rel.oid AS indexrelid,
  ns.nspname AS schema,
  rel.relname AS indexrelname,
  db.datname AS dbname,
  stats.wal_records,
  stats.wal_fpi,
  stats.wal_bytes,
  stats.tuples_deleted,
  stats.pages_deleted
FROM pg_database db,
     pg_class rel,
     pg_namespace ns,
     LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_indexes(db.oid, rel.oid) stats
WHERE db.datname = current_database()
  AND rel.relkind = 'i'
  AND rel.relnamespace = ns.oid
  AND rel.oid = stats.relid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_indexes IS
  'Extended vacuum statistics per index';

-- View: vacuum statistics per database (aggregate)
CREATE VIEW ext_vacuum_statistics.pg_stats_vacuum_database AS
SELECT
  db.oid AS dboid,
  db.datname AS dbname,
  stats.wal_records AS db_wal_records,
  stats.wal_fpi AS db_wal_fpi,
  stats.wal_bytes AS db_wal_bytes
FROM pg_database db
LEFT JOIN LATERAL ext_vacuum_statistics.pg_stats_get_vacuum_database(db.oid) stats ON db.oid = stats.dbid;

COMMENT ON VIEW ext_vacuum_statistics.pg_stats_vacuum_database IS
  'Extended vacuum statistics per database (aggregate)';
