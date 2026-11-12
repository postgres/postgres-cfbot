# Test that heap rewrites and index builds cope with RECENTLY_DEAD tuples
# whose TOAST data has already been removed.
#
# A transaction in another database holds back the xmin of the rewriting
# backend, but not the removal horizon of this database.  The main table and
# its TOAST table are vacuumed separately, with a same-database snapshot that
# keeps the dead tuple released in between, so the tuple survives while its
# TOAST chunks are removed.

setup
{
    CREATE DATABASE regress_other_db;
}

setup
{
    CREATE EXTENSION IF NOT EXISTS dblink;

    CREATE TABLE rewrite_test (id int PRIMARY KEY, data text)
        WITH (autovacuum_enabled = off);
    ALTER TABLE rewrite_test ALTER COLUMN data SET STORAGE EXTERNAL;
    INSERT INTO rewrite_test VALUES (1, repeat('x', 2500));
    INSERT INTO rewrite_test VALUES (2, repeat('y', 2500));

    -- Rename the toast table to a known name so we can VACUUM it directly.
    SET allow_system_table_mods TO true;
    DO $$DECLARE r record;
      BEGIN
        SELECT INTO r reltoastrelid::regclass::text AS table_name
          FROM pg_class WHERE oid = 'rewrite_test'::regclass;
        EXECUTE 'ALTER TABLE ' || r.table_name || ' RENAME TO rewrite_test_toast;';
      END$$;
    RESET allow_system_table_mods;
}

teardown
{
    DROP DATABASE regress_other_db;
}

# Session s1: use dblink to hold a transaction in another database.
# This must start before the DELETE so its XID is older, ensuring
# GetSnapshotData() holds back any rewrites.
session s1
step s1_hold_xmin
{
    SELECT dblink_connect('holder',
        'dbname=regress_other_db port=' || current_setting('port'));
    SELECT dblink_exec('holder', 'BEGIN');
    SELECT dblink_exec('holder', 'CREATE TEMP TABLE xid_holder(x int)');
}
step s1_release
{
    SELECT dblink_exec('holder', 'COMMIT');
    SELECT dblink_disconnect('holder');
}

# Session s2: holds xmin in the SAME database temporarily, so the
# main-table vacuum preserves the dead tuple as RECENTLY_DEAD.
session s2
step s2_begin
{
    BEGIN ISOLATION LEVEL REPEATABLE READ;
    SELECT 1;
}
step s2_commit { COMMIT; }

# Session s3: performs DML, split vacuum, and the rewrite.
session s3
step s3_delete
{
    DELETE FROM rewrite_test WHERE id = 1;
}
step s3_update
{
    UPDATE rewrite_test SET data = repeat('z', 2500) WHERE id = 1;
}
step s3_update_again
{
    UPDATE rewrite_test SET data = repeat('w', 2500) WHERE id = 1;
}
step s3_check_rows
{
    SELECT id, length(data), left(data, 1) FROM rewrite_test ORDER BY id;
    SELECT reltuples FROM pg_class WHERE oid = 'rewrite_test'::regclass;
}
step s3_vacuum_main_only
{
    VACUUM (PROCESS_TOAST false) rewrite_test;
}
step s3_vacuum_toast
{
    VACUUM pg_toast.rewrite_test_toast;
}
step s3_cluster
{
    CLUSTER rewrite_test USING rewrite_test_pkey;
}
# Use the seqscan-and-sort strategy, which fetches TOAST data before sorting
step s3_cluster_sort
{
    SET enable_indexscan = off;
    CLUSTER rewrite_test USING rewrite_test_pkey;
    RESET enable_indexscan;
}
step s3_vacuum_full
{
    VACUUM FULL rewrite_test;
}
step s3_repack
{
    REPACK rewrite_test;
}
step s3_create_index
{
    CREATE INDEX rewrite_test_data_idx ON rewrite_test (data);
}
step s3_create_index_wholerow
{
    CREATE INDEX rewrite_test_row_idx ON rewrite_test ((rewrite_test IS NOT NULL));
}

teardown { DROP TABLE IF EXISTS rewrite_test; }

# CLUSTER
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_cluster
    s1_release

# CLUSTER, sorting
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_cluster_sort
    s3_check_rows
    s1_release

# VACUUM FULL
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_vacuum_full
    s1_release

# REPACK
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_repack
    s1_release

# CREATE INDEX
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_create_index
    s1_release

# CREATE INDEX on a whole-row expression
permutation
    s1_hold_xmin
    s2_begin
    s3_delete
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_create_index_wholerow
    s1_release

# Preserve the live successor when a stashed prior version loses its TOAST.
permutation
    s1_hold_xmin
    s2_begin
    s3_update
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_vacuum_full
    s3_check_rows
    s1_release

permutation
    s1_hold_xmin
    s2_begin
    s3_update
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_repack
    s3_check_rows
    s1_release

permutation
    s1_hold_xmin
    s2_begin
    s3_update
    s3_update_again
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_vacuum_full
    s3_check_rows
    s1_release

permutation
    s1_hold_xmin
    s2_begin
    s3_update
    s3_update_again
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_repack
    s3_check_rows
    s1_release

permutation
    s1_hold_xmin
    s2_begin
    s3_update
    s3_update_again
    s3_vacuum_main_only
    s2_commit
    s3_vacuum_toast
    s3_cluster_sort
    s3_check_rows
    s1_release
