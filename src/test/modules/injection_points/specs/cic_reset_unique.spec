# Test uniqueness checks across snapshot resets.
#
# The scan collects equal keys under different snapshots while a delete
# remains uncommitted.  The build defers the conflict to validation,
# which must succeed if the delete commits and fail if it rolls back.
# An uncommitted HOT update must also defer the check, but leaves duplicates
# whether it commits or rolls back.
#
# Pause before a snapshot reset to insert the second row, then at
# bt_load_defer_unique_check to control when the delete or update finishes.

setup
{
    CREATE EXTENSION injection_points;
    CREATE TABLE cic_uniq (k int, pad text);
    ALTER TABLE cic_uniq ALTER COLUMN pad SET STORAGE PLAIN;
    ALTER TABLE cic_uniq SET (parallel_workers = 0);
    -- Rows are padded to the largest multiple of 8 that fits 7 times into the
    -- space a page has for tuples (header 24, line pointer 4, row header + int
    -- + varlena header 32): 7 rows per page whatever the block size, with no
    -- room left for another row.  Free one slot on a page the scan reaches
    -- late, so that the row inserted during the build lands on a page the scan
    -- has not read yet.
    INSERT INTO cic_uniq
        SELECT g, repeat('x', ((current_setting('block_size')::int - 24) / 7 - 4) / 8 * 8 - 32)
        FROM generate_series(1, 35) g;
    DELETE FROM cic_uniq WHERE k = 25;
}

teardown
{
    DROP TABLE cic_uniq;
    DROP EXTENSION injection_points;
}

session s1
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('heap_reset_scan_snapshot_swap', 'wait');
    SELECT injection_points_attach('bt_load_defer_unique_check', 'wait');
    SET concurrent_index_reset_snapshot_interval = 0;
    SET index_build_duplicate_check_scale_factor = 1;
}
step zero_budget { SET index_build_duplicate_check_scale_factor = 0; }
step build { CREATE UNIQUE INDEX CONCURRENTLY cic_uniq_idx ON cic_uniq(k); }

session s2
step s2_begin    { BEGIN; }
step s2_delete   { DELETE FROM cic_uniq WHERE k = 1; }
step s2_hot_update {
    UPDATE cic_uniq SET pad = overlay(pad placing 'y' from 1 for 1) WHERE k = 1;
    SELECT n_tup_upd, n_tup_hot_upd FROM pg_stat_xact_user_tables
        WHERE relid = 'cic_uniq'::regclass;
}
step s2_commit   { COMMIT; }
step s2_rollback { ROLLBACK; }

session s3
# Make room for a HOT update of the first row before the build starts.
step s3_free_hot_slot { DELETE FROM cic_uniq WHERE k = 2; }
# Record the freed slot in the FSM so the new row stays within the
# scan's block range, which is fixed when the scan starts.
step s3_vacuum { VACUUM cic_uniq; }
step s3_wake   { SELECT injection_points_wakeup('heap_reset_scan_snapshot_swap'); }
step s3_insert { INSERT INTO cic_uniq VALUES (1, 'y'); }
# Let the scan run to its end; the build then stops at the deferral point.
step s3_scan_end {
    SELECT injection_points_detach('heap_reset_scan_snapshot_swap');
    SELECT injection_points_wakeup('heap_reset_scan_snapshot_swap');
}
# Wait for the build to defer the uniqueness check before ending the
# deleting or updating transaction.
step s3_await_defer {
    DO $$
    DECLARE tries int := 0;
    BEGIN
        LOOP
            PERFORM 1 FROM pg_stat_activity
             WHERE wait_event = 'bt_load_defer_unique_check';
            EXIT WHEN FOUND;
            IF tries >= 1200 THEN
                RAISE 'the build did not reach the deferral point';
            END IF;
            PERFORM pg_sleep(0.05);
            tries := tries + 1;
        END LOOP;
    END $$;
}
# Let the build finish and reach the validation phase.
step s3_build_end {
    SELECT injection_points_detach('bt_load_defer_unique_check');
    SELECT injection_points_wakeup('bt_load_defer_unique_check');
}

# Committing the delete leaves unique data, so the build must succeed.
permutation s3_vacuum build s3_wake s2_begin s2_delete s3_insert s3_scan_end s3_await_defer s2_commit s3_build_end

# Rolling back the delete leaves duplicates, so the build must fail.
permutation s3_vacuum build s3_wake s2_begin s2_delete s3_insert s3_scan_end s3_await_defer s2_rollback s3_build_end

# Neither outcome of a HOT update removes the conflicting key.
permutation s3_free_hot_slot s3_vacuum build s3_wake s2_begin s2_hot_update s3_insert s3_scan_end s3_await_defer s2_commit s3_build_end
permutation s3_free_hot_slot s3_vacuum build s3_wake s2_begin s2_hot_update s3_insert s3_scan_end s3_await_defer s2_rollback s3_build_end

# With no heap fetches, validation must insert the surviving row after a
# committed delete, and reject the duplicates after a rollback.
permutation zero_budget s3_vacuum build s3_wake s2_begin s2_delete s3_insert s3_scan_end s3_await_defer s2_commit s3_build_end
permutation zero_budget s3_vacuum build s3_wake s2_begin s2_delete s3_insert s3_scan_end s3_await_defer s2_rollback s3_build_end
