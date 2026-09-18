-- Test old extension version entry points.

CREATE EXTENSION pg_walinspect WITH VERSION '1.0';

-- Mask DETAIL messages as these could refer to current LSN positions.
\set VERBOSITY terse

-- List what version 1.0 contains, using a locale-independent sorting.
SELECT pg_describe_object(classid, objid, 0) AS obj
  FROM pg_depend
  WHERE refclassid = 'pg_extension'::regclass AND
    refobjid = (SELECT oid FROM pg_extension
                  WHERE extname = 'pg_walinspect') AND deptype = 'e'
  ORDER BY pg_describe_object(classid, objid, 0) COLLATE "C";

-- Make sure checkpoints don't interfere with the test.
SELECT 'init' FROM pg_create_physical_replication_slot('regress_pg_walinspect_slot', true, false);

CREATE TABLE sample_tbl(col1 int, col2 int);
SELECT pg_current_wal_lsn() AS wal_lsn1 \gset
INSERT INTO sample_tbl SELECT * FROM generate_series(1, 2);

-- Tests for the past functions.
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_records_info_till_end_of_wal(:'wal_lsn1');
SELECT COUNT(*) >= 1 AS ok FROM pg_get_wal_stats_till_end_of_wal(:'wal_lsn1');
-- Failures with start LSNs.
SELECT * FROM pg_get_wal_records_info_till_end_of_wal('FFFFFFFF/FFFFFFFF');
SELECT * FROM pg_get_wal_stats_till_end_of_wal('FFFFFFFF/FFFFFFFF');

-- Test each upgrade step through the current version.
ALTER EXTENSION pg_walinspect UPDATE TO '1.1';
ALTER EXTENSION pg_walinspect UPDATE TO '1.2';

-- List what the current version contains.
\dx+ pg_walinspect

SELECT pg_drop_replication_slot('regress_pg_walinspect_slot');

DROP TABLE sample_tbl;
DROP EXTENSION pg_walinspect;
