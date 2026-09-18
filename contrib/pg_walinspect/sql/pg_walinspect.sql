CREATE EXTENSION pg_walinspect;

-- Mask DETAIL messages as these could refer to current LSN positions.
\set VERBOSITY terse

-- Make sure checkpoints don't interfere with the test.
SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_walinspect_slot', true, false);

CREATE TABLE sample_tbl(col1 int, col2 int);

-- Save some LSNs for comparisons.
SELECT pg_current_wal_lsn() AS wal_lsn1 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);
SELECT pg_current_wal_lsn() AS wal_lsn2 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(3, 4);

-- ===================================================================
-- Tests for input validation
-- ===================================================================

-- Invalid input LSN.
SELECT * FROM pg_get_wal_record_info('0/0');

-- Invalid start LSN.
SELECT * FROM pg_get_wal_records_info('0/0', :'wal_lsn1');
SELECT * FROM pg_get_wal_stats('0/0', :'wal_lsn1');
SELECT * FROM pg_get_wal_block_info('0/0', :'wal_lsn1');

-- Start LSN > End LSN.
SELECT * FROM pg_get_wal_records_info(:'wal_lsn2', :'wal_lsn1');
SELECT * FROM pg_get_wal_stats(:'wal_lsn2', :'wal_lsn1');
SELECT * FROM pg_get_wal_block_info(:'wal_lsn2', :'wal_lsn1');

-- LSNs with the highest value possible.
SELECT * FROM pg_get_wal_record_info('FFFFFFFF/FFFFFFFF');
-- Success with end LSNs.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn1', 'FFFFFFFF/FFFFFFFF');
-- Failures with start LSNs.
SELECT * FROM pg_get_wal_records_info('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_stats('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_block_info('FFFFFFFF/FFFFFFFE', 'FFFFFFFF/FFFFFFFF');

-- ===================================================================
-- Tests for all function executions
-- ===================================================================

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_record_info(:'wal_lsn1');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats(:'wal_lsn1', :'wal_lsn2');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn1', :'wal_lsn2');

-- Return the retained WAL file and its complete segment boundaries.
SELECT wal_file = pg_walfile_name(segment_start_lsn) AS file_ok,
       segment_start_lsn <= :'wal_lsn1'::pg_lsn AS start_ok,
       segment_end_lsn > :'wal_lsn1'::pg_lsn AS end_ok
FROM pg_get_wal_files(:'wal_lsn1', :'wal_lsn1'::pg_lsn + 1);

-- An end LSN at a segment boundary must not include the next segment.
WITH segment AS
(
  SELECT * FROM pg_get_wal_files(:'wal_lsn1', :'wal_lsn1'::pg_lsn + 1)
)
SELECT count(*) = 1 AS one_file
FROM segment,
     LATERAL pg_get_wal_files(segment_start_lsn, segment_end_lsn);

SELECT count(*) = 1 AS equal_lsn_ok
FROM pg_get_wal_files(:'wal_lsn1', :'wal_lsn1');
SELECT count(*) = 1 AS default_end_ok
FROM pg_get_wal_files(:'wal_lsn1');
SELECT * FROM pg_get_wal_files(:'wal_lsn1'::pg_lsn + 1, :'wal_lsn1');
SELECT * FROM pg_get_wal_files(pg_current_wal_lsn(), 'FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_files('0/0', '0/1');

-- ===================================================================
-- Tests for locating WAL by timestamps stored in WAL records
-- ===================================================================

-- Put a target timestamp between two WAL-logged transaction commits.
INSERT INTO sample_tbl VALUES (5, 5);
SELECT clock_timestamp() AS wal_time_target \gset
INSERT INTO sample_tbl VALUES (6, 6);

SELECT start_timestamp <= :'wal_time_target'::timestamptz AS start_ok,
       end_timestamp >= :'wal_time_target'::timestamptz AS end_ok,
       start_lsn < end_lsn AS lsn_ok
FROM pg_get_wal_location_at_time(:'wal_time_target',
                                 interval '1 microsecond',
                                 interval '1 microsecond');

SELECT start_timestamp <= :'wal_time_target'::timestamptz - interval '1 microsecond'
         AS start_ok,
       end_timestamp >= :'wal_time_target'::timestamptz AS end_ok,
       start_lsn < end_lsn AS lsn_ok
FROM pg_get_wal_location_at_time(:'wal_time_target',
                                 before => interval '1 microsecond',
                                 after => interval '1 microsecond');

SELECT start_timestamp <= :'wal_time_target'::timestamptz AS start_ok,
       end_timestamp >= :'wal_time_target'::timestamptz + interval '1 microsecond'
         AS end_ok,
       start_lsn < end_lsn AS lsn_ok
FROM pg_get_wal_location_at_time(:'wal_time_target',
                                 before => interval '1 microsecond',
                                 after => interval '1 microsecond');

SELECT start_timestamp <= :'wal_time_target'::timestamptz - interval '1 microsecond'
         AS start_ok,
       end_timestamp >= :'wal_time_target'::timestamptz + interval '1 microsecond'
         AS end_ok,
       start_lsn < end_lsn AS lsn_ok
FROM pg_get_wal_location_at_time(:'wal_time_target',
                                 before => interval '1 microsecond',
                                 after => interval '1 microsecond');

-- COMMIT PREPARED records also provide commit-time anchors.
BEGIN;
INSERT INTO sample_tbl VALUES (7, 7);
PREPARE TRANSACTION 'regress_pg_walinspect_time';
SELECT clock_timestamp() AS prepared_time_target \gset
COMMIT PREPARED 'regress_pg_walinspect_time';

SELECT record_type = 'COMMIT_PREPARED' AS prepared_ok
FROM pg_get_wal_location_at_time(:'prepared_time_target',
                                 interval '1 microsecond',
                                 interval '1 microsecond') AS location,
     LATERAL pg_get_wal_record_info(location.end_lsn);

-- Non-transaction WAL records with timestamps can also be anchors.
SELECT pg_create_restore_point('regress_wal_time_lower') AS restore_lsn \gset
SELECT clock_timestamp() AS restore_time_target \gset
SELECT pg_create_restore_point('regress_wal_time_upper') AS restore_lsn \gset
INSERT INTO sample_tbl VALUES (8, 8);

SELECT start_info.record_type = 'RESTORE_POINT' AS start_ok,
       end_info.record_type = 'RESTORE_POINT' AS end_ok
FROM pg_get_wal_location_at_time(:'restore_time_target',
                                 interval '1 microsecond',
                                 interval '1 microsecond') AS location,
     LATERAL pg_get_wal_record_info(location.start_lsn) AS start_info,
     LATERAL pg_get_wal_record_info(location.end_lsn) AS end_info;

-- A future upper bound uses a timestamped WAL record at or before now.
SELECT clock_timestamp() AS current_time_target \gset
INSERT INTO sample_tbl VALUES (9, 9);
SELECT location.start_timestamp <= :'current_time_target'::timestamptz - interval '1 microsecond'
         AS start_ok,
       location.end_timestamp >= :'current_time_target'::timestamptz AND
         location.end_timestamp <= clock_timestamp() AS capped_end_ok,
       location.start_lsn < location.end_lsn AS lsn_ok,
       end_info.record_type = 'COMMIT' AS real_record_ok
FROM pg_get_wal_location_at_time(:'current_time_target',
                                 interval '1 microsecond',
                                 interval '1 day') AS location,
     LATERAL pg_get_wal_record_info(location.end_lsn) AS end_info;

SELECT pg_get_function_arguments(
  'pg_get_wal_location_at_time(timestamptz, interval, interval)'::regprocedure)
  LIKE '%before interval DEFAULT ''@ 1 min''::interval, after interval DEFAULT ''@ 1 min''::interval%'
  AS defaults_ok;
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target',
                                         interval '0', interval '1 second');
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target',
                                         interval '1 second', interval '0');
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target', interval '-1 second');
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target',
                                         after => interval '-1 second');
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target',
                                         before => interval '1 day 1 microsecond');
SELECT * FROM pg_get_wal_location_at_time(:'wal_time_target',
                                         after => interval '1 day 1 microsecond');
SELECT * FROM pg_get_wal_location_at_time(clock_timestamp() - interval '100 years');
SELECT * FROM pg_get_wal_location_at_time(clock_timestamp() + interval '100 years');

-- ===================================================================
-- Test for filtering out WAL records of a particular table
-- ===================================================================

SELECT oid AS sample_tbl_oid FROM pg_class WHERE relname = 'sample_tbl' \gset

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2')
			WHERE block_ref LIKE concat('%', :'sample_tbl_oid', '%') AND resource_manager = 'Heap';

-- ===================================================================
-- Test for filtering out WAL records based on resource_manager and
-- record_type
-- ===================================================================

SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info(:'wal_lsn1', :'wal_lsn2')
			WHERE resource_manager = 'Heap' AND record_type = 'INSERT';

-- ===================================================================
-- Tests to get block information from WAL record
-- ===================================================================

-- Update table to generate some block data.
SELECT pg_current_wal_lsn() AS wal_lsn3 \gset
UPDATE sample_tbl SET col1 = col1 + 1 WHERE col1 = 1;
SELECT pg_current_wal_lsn() AS wal_lsn4 \gset
-- Check if we get block data from WAL record.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn3', :'wal_lsn4')
  WHERE relfilenode = :'sample_tbl_oid' AND block_data IS NOT NULL;

-- Force a checkpoint so that the next update will log a full-page image.
SELECT pg_current_wal_lsn() AS wal_lsn5 \gset
CHECKPOINT;

-- Verify that an XLOG_CHECKPOINT_REDO record begins at precisely the redo LSN
-- of the checkpoint we just performed.
SELECT redo_lsn FROM pg_control_checkpoint() \gset
SELECT start_lsn = :'redo_lsn'::pg_lsn AS same_lsn, resource_manager,
    record_type FROM pg_get_wal_record_info(:'redo_lsn');

-- This update should produce a full-page image because of the checkpoint.
UPDATE sample_tbl SET col1 = col1 + 1 WHERE col1 = 2;
SELECT pg_current_wal_lsn() AS wal_lsn6 \gset
-- Check if we get FPI from WAL record.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_block_info(:'wal_lsn5', :'wal_lsn6')
  WHERE relfilenode = :'sample_tbl_oid' AND block_fpi_data IS NOT NULL;

-- ===================================================================
-- Tests for permissions
-- ===================================================================
CREATE ROLE regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_location_at_time(timestamptz, interval, interval)', 'EXECUTE'); -- no
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_files(pg_lsn, pg_lsn)', 'EXECUTE'); -- no

-- Functions accessible by users with role pg_read_server_files.
GRANT pg_read_server_files TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_location_at_time(timestamptz, interval, interval)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_files(pg_lsn, pg_lsn)', 'EXECUTE'); -- yes

REVOKE pg_read_server_files FROM regress_pg_walinspect;

-- Superuser can grant execute to other users.
GRANT EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_location_at_time(timestamptz, interval, interval)
  TO regress_pg_walinspect;
GRANT EXECUTE ON FUNCTION pg_get_wal_files(pg_lsn, pg_lsn)
  TO regress_pg_walinspect;

SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_record_info(pg_lsn)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_records_info(pg_lsn, pg_lsn) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_stats(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_block_info(pg_lsn, pg_lsn, boolean) ', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_location_at_time(timestamptz, interval, interval)', 'EXECUTE'); -- yes
SELECT has_function_privilege('regress_pg_walinspect',
  'pg_get_wal_files(pg_lsn, pg_lsn)', 'EXECUTE'); -- yes

REVOKE EXECUTE ON FUNCTION pg_get_wal_record_info(pg_lsn)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_records_info(pg_lsn, pg_lsn)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_stats(pg_lsn, pg_lsn, boolean)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_block_info(pg_lsn, pg_lsn, boolean)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_location_at_time(timestamptz, interval, interval)
  FROM regress_pg_walinspect;
REVOKE EXECUTE ON FUNCTION pg_get_wal_files(pg_lsn, pg_lsn)
  FROM regress_pg_walinspect;

-- ===================================================================
-- Clean up
-- ===================================================================

DROP ROLE regress_pg_walinspect;

SELECT pg_drop_replication_slot('regress_pg_walinspect_slot');

DROP TABLE sample_tbl;
DROP EXTENSION pg_walinspect;
