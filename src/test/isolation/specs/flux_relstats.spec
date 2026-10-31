# The relation statistics VACUUM, ANALYZE and the UPDATE counters give the
# planner and autovacuum, for heap and FLUX side by side.
#
# - VACUUM or ANALYZE while another transaction deletes and inserts rows: as
#   for heap, rows being inserted are not counted yet and rows being deleted
#   are still live, because the writer's commit reports both to pgstat.  So
#   after the commit n_live_tup and n_dead_tup are the real counts.
# - UPDATE counters: n_tup_upd and n_mod_since_analyze count an UPDATE once,
#   whether FLUX does it in place or at a new TID, and it is not also counted
#   as an INSERT and a DELETE.  n_dead_tup counts the versions VACUUM will
#   have to remove: heap leaves the old version of each row; a FLUX in-place
#   UPDATE of a non-indexed column leaves none (its before-image is in UNDO,
#   which VACUUM does not reclaim); a FLUX in-place key UPDATE leaves a
#   delete-marked index entry and an out-of-place one a deleted version,
#   both for VACUUM.  A FLUX non-key UPDATE is counted as HOT: it touches no
#   index.
# - ANALYZE counts no dead row for a FLUX in-place UPDATE either (permutation
#   1 and 2 cover what it counts for in-progress DML).

setup
{
  CREATE FUNCTION st_count(tbl regclass) RETURNS bigint LANGUAGE plpgsql AS $$
  DECLARE n bigint;
  BEGIN EXECUTE format('SELECT count(*) FROM %s', tbl) INTO n; RETURN n; END $$;
  CREATE TABLE st_heap (a int PRIMARY KEY, b int, c bigint) USING heap WITH (autovacuum_enabled = off);
  CREATE TABLE st_flux (a int PRIMARY KEY, b int, c bigint) USING flux WITH (autovacuum_enabled = off);
  CREATE TABLE sh_heap (a int, c bigint) USING heap WITH (autovacuum_enabled = off);
  CREATE TABLE sh_flux (a int, c bigint) USING flux WITH (autovacuum_enabled = off);
  CREATE INDEX ON sh_heap USING hash (a);
  CREATE INDEX ON sh_flux USING hash (a);
  INSERT INTO st_heap SELECT g, g, g FROM generate_series(1, 10000) g;
  INSERT INTO st_flux SELECT g, g, g FROM generate_series(1, 10000) g;
  INSERT INTO sh_heap SELECT g, g FROM generate_series(1, 10000) g;
  INSERT INTO sh_flux SELECT g, g FROM generate_series(1, 10000) g;
  SELECT pg_stat_force_next_flush();
}
setup { VACUUM ANALYZE st_heap; }
setup { VACUUM ANALYZE st_flux; }
setup { VACUUM ANALYZE sh_heap; }
setup { VACUUM ANALYZE sh_flux; }

teardown
{
  DROP TABLE st_heap, st_flux, sh_heap, sh_flux;
  DROP FUNCTION st_count;
}

session s1
step s1_dml
{
  BEGIN;
  DELETE FROM st_heap WHERE a <= 1000;
  DELETE FROM st_flux WHERE a <= 1000;
  INSERT INTO st_heap SELECT g, g, g FROM generate_series(20001, 22000) g;
  INSERT INTO st_flux SELECT g, g, g FROM generate_series(20001, 22000) g;
}
step s1_commit { COMMIT; SELECT pg_stat_force_next_flush(); }

session s2
step s2_vacuum_heap { VACUUM st_heap; }
step s2_vacuum_flux { VACUUM st_flux; }
step s2_analyze { ANALYZE st_heap, st_flux; }
# 10000 committed rows were live when VACUUM or ANALYZE ran (heap's VACUUM
# may extrapolate from the pages it scanned, hence the tolerance).
step s2_reltuples
{
  SELECT relname, abs(reltuples - 10000) <= 500 AS committed_rows_only
  FROM pg_class WHERE relname IN ('st_heap', 'st_flux') ORDER BY relname;
}
step s2_pgstat
{
  SELECT pg_stat_clear_snapshot();
  SELECT relname, abs(n_live_tup - st_count(relid)) <= 500 AS live_ok,
         n_dead_tup = 1000 AS dead_ok
  FROM pg_stat_user_tables WHERE relname IN ('st_heap', 'st_flux')
  ORDER BY relname;
}
step s2_update_nonkey
{
  UPDATE st_heap SET c = c + 1;
  UPDATE st_flux SET c = c + 1;
  SELECT pg_stat_force_next_flush();
}
step s2_update_key
{
  UPDATE st_heap SET a = a + 100000;
  UPDATE st_flux SET a = a + 100000;
  SELECT pg_stat_force_next_flush();
}
step s2_update_hashkey
{
  UPDATE sh_heap SET a = a + 100000;
  UPDATE sh_flux SET a = a + 100000;
  SELECT pg_stat_force_next_flush();
}
step s2_update_counts
{
  SELECT pg_stat_clear_snapshot();
  SELECT relname, n_tup_upd = 10000 AS upd_ok,
         n_tup_ins = 10000 AND n_tup_del = 0 AS not_ins_del,
         n_mod_since_analyze = 10000 AS mod_ok,
         n_tup_hot_upd = n_tup_upd AS all_hot,
         CASE n_dead_tup WHEN 0 THEN 'none' WHEN 10000 THEN 'all'
              ELSE 'some' END AS dead
  FROM pg_stat_user_tables
  WHERE relname IN ('st_heap', 'st_flux', 'sh_heap', 'sh_flux')
    AND n_tup_upd + n_tup_del > 0
  ORDER BY relname;
}
step s2_dead
{
  SELECT pg_stat_clear_snapshot();
  SELECT relname, CASE n_dead_tup WHEN 0 THEN 'none' WHEN 10000 THEN 'all'
                  ELSE 'some' END AS dead
  FROM pg_stat_user_tables WHERE relname IN ('st_heap', 'st_flux')
  ORDER BY relname;
}

permutation s1_dml s2_vacuum_heap s2_vacuum_flux s2_reltuples s1_commit s2_pgstat
permutation s1_dml s2_analyze s2_reltuples s1_commit s2_pgstat
permutation s2_update_nonkey s2_update_counts s2_analyze s2_dead
permutation s2_update_key s2_update_counts
permutation s2_update_hashkey s2_update_counts
