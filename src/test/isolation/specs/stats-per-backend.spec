# Test snapshot, cache, and reset behavior for per-backend statistics.
#
# Session s2 generates and flushes WAL records, which provide a deterministic
# per-backend counter. Session s1 verifies that SNAPSHOT mode fixes all
# per-backend entries at the initial statistics snapshot, while CACHE mode
# fixes each entry on first access and refreshes it after the transaction
# ends. The final permutations verify reset timestamps after per-backend and
# shared resets.

setup
{
  CREATE TABLE stats_per_backend_data(id int);
  CREATE TABLE stats_per_backend_saved(wal_records bigint);
}

teardown
{
  DROP TABLE stats_per_backend_data;
  DROP TABLE stats_per_backend_saved;
}

session s1
setup { SET stats_fetch_consistency = 'none'; }

step s1_fetch_consistency_cache { SET stats_fetch_consistency = 'cache'; }
step s1_fetch_consistency_snapshot { SET stats_fetch_consistency = 'snapshot'; }
step s1_begin { BEGIN; }
step s1_commit { COMMIT; }
step s1_build_snapshot {
  SELECT wal_records >= 0 AS snapshot_created FROM pg_stat_wal;
}
step s1_check_snapshot_backend {
  SELECT wal_records = 0 AS snapshot_excludes_later_update
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}
step s1_check_live_backend {
  SELECT wal_records > 0 AS live_entry_exists
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}
step s1_save_backend_stats {
  INSERT INTO stats_per_backend_saved
  SELECT wal_records
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}
step s1_check_cached_backend {
  SELECT current.wal_records = saved.wal_records AS cache_is_stable
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2')) AS current
  CROSS JOIN stats_per_backend_saved AS saved;
}
step s1_check_refreshed_backend {
  SELECT current.wal_records > saved.wal_records AS cache_is_refreshed
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2')) AS current
  CROSS JOIN stats_per_backend_saved AS saved;
}
step s1_reset_backend {
  SELECT pg_stat_reset_backend_stats(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}
step s1_check_backend_reset {
  SELECT stats_reset IS NOT NULL AS reset_timestamp_set
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}
step s1_reset_shared {
  SELECT pg_stat_reset_shared('wal');
}
step s1_check_shared_reset {
  SELECT stats_reset IS NOT NULL AS reset_timestamp_set
  FROM pg_stat_get_backend_wal(
    (SELECT pid FROM pg_stat_activity
     WHERE application_name = 'isolation/stats-per-backend/s2'));
}

session s2
step s2_generate {
  INSERT INTO stats_per_backend_data VALUES (1);
  SELECT pg_stat_force_next_flush();
}
step s2_generate_more {
  INSERT INTO stats_per_backend_data VALUES (2);
  SELECT pg_stat_force_next_flush();
}

# with stats_fetch_consistency=snapshot s1 should not see flushed changes from
# s2 after building the statistics snapshot, but should see them after commit
permutation
  s1_fetch_consistency_snapshot
  s1_begin
  s1_build_snapshot
  s2_generate
  s1_check_snapshot_backend
  s1_commit
  s1_check_live_backend

# with stats_fetch_consistency=cache s1 should not see flushed changes from s2
# after the first access, but should see them after commit
permutation
  s1_fetch_consistency_cache
  s2_generate
  s1_begin
  s1_save_backend_stats
  s2_generate_more
  s1_check_cached_backend
  s1_commit
  s1_check_refreshed_backend

# a per-backend reset should set that backend's reset timestamp
permutation s1_reset_backend s1_check_backend_reset

# a shared WAL reset should set the reset timestamp for live backends
permutation s1_reset_shared s1_check_shared_reset
