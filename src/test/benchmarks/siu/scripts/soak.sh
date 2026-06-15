#!/usr/bin/env bash
# tepid soak: run hot_indexed_update for $DURATION seconds on each variant, sampling
# TPS / HOT-rate / WAL volume / table+index bloat every $SAMPLE seconds.
# Emits a CSV with one sample row per tick per variant.
set -euo pipefail

BENCH=${BENCH:-/scratch/siu-bench}
SCALE=${SCALE:-50}
CLIENTS=${CLIENTS:-16}
THREADS=${THREADS:-8}
DURATION=${DURATION:-900}     # 15 minutes
SAMPLE=${SAMPLE:-60}          # every 60 s
PORT=${PORT:-57503}
SHARED_BUFFERS=${SHARED_BUFFERS:-2GB}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$BENCH/results/soak_$TS.csv
LOGDIR=$BENCH/logs/soak_$TS
mkdir -p "$LOGDIR" "$BENCH/results"
echo "variant,t_secs,tps_instant,hot_pct_instant,heap_pages,index_bytes,wal_bytes_since_start,n_dead_tup" > "$OUT"
echo "=== soak $TS -> $OUT"

bin_of()  { echo "$BENCH/$1/usr/local/pgsql/bin"; }
LD_of()   { local b=$BENCH/$1/usr/local/pgsql; [ -d "$b/lib64" ] && echo "$b/lib64" || echo "$b/lib"; }

psql_as() { local v=$1; shift; LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/psql" -h /tmp -p "$PORT" -U postgres -X "$@"; }
pgbench_as() { local v=$1; shift; LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pgbench" -h /tmp -p "$PORT" -U postgres "$@"; }

start_pg() {
  local v=$1 datadir=$BENCH/_data_$v
  [ -d "$datadir" ] && find "$datadir" -mindepth 1 -delete && rmdir "$datadir"
  mkdir -p "$datadir"
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/initdb" -D "$datadir" -U postgres >"$LOGDIR/initdb_$v.log" 2>&1
  cat >> "$datadir/postgresql.conf" <<EOF
shared_buffers = $SHARED_BUFFERS
work_mem = 32MB
max_wal_size = 8GB
synchronous_commit = on
checkpoint_timeout = 10min
wal_level = replica
autovacuum = on
autovacuum_naptime = 10s
autovacuum_vacuum_threshold = 50
autovacuum_vacuum_scale_factor = 0.1
port = $PORT
EOF
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$datadir" \
    -o "-p $PORT" -l "$LOGDIR/pg_$v.log" start >/dev/null
  sleep 2
}

stop_pg() {
  local v=$1
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$BENCH/_data_$v" stop -m fast >/dev/null 2>&1 || true
}

setup() {
  local v=$1 rows=$((SCALE * 100000))
  psql_as "$v" <<SQL
DROP TABLE IF EXISTS siu_table;
CREATE TABLE siu_table(a int PRIMARY KEY, b int, c int, d int, e text);
CREATE INDEX siu_b ON siu_table(b);
CREATE INDEX siu_c ON siu_table(c);
CREATE INDEX siu_d ON siu_table(d);
INSERT INTO siu_table
  SELECT i, i, i, i, repeat('x', 20) FROM generate_series(1, $rows) AS i;
VACUUM (ANALYZE) siu_table;
SQL
}

run_soak() {
  local v=$1
  echo "--- soak $v for ${DURATION}s, sampling every ${SAMPLE}s"
  stop_pg "$v" || true
  start_pg "$v"
  setup "$v"
  local wal0
  wal0=$(psql_as "$v" -Atc "SELECT pg_current_wal_lsn()::text")
  local hot0 tot0
  hot0=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_hot_upd,0) FROM pg_stat_user_tables WHERE relname='siu_table'")
  tot0=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_upd,0)     FROM pg_stat_user_tables WHERE relname='siu_table'")
  local prev_hot=$hot0 prev_tot=$tot0

  # Drive pgbench in the background; sampler in foreground.
  pgbench_as "$v" -f "$BENCH/scripts/hot_indexed_update.sql" \
    -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
    -P "$SAMPLE" -n postgres >"$LOGDIR/pgbench_$v.log" 2>&1 &
  local pgb=$!

  local t=0
  while [ "$t" -lt "$DURATION" ]; do
    sleep "$SAMPLE"
    t=$((t + SAMPLE))
    local now_hot now_tot wal_now wal_bytes heap_pages idx_bytes n_dead
    now_hot=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_hot_upd,0) FROM pg_stat_user_tables WHERE relname='siu_table'")
    now_tot=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_upd,0)     FROM pg_stat_user_tables WHERE relname='siu_table'")
    wal_now=$(psql_as "$v" -Atc "SELECT pg_current_wal_lsn()::text")
    wal_bytes=$(psql_as "$v" -Atc "SELECT pg_wal_lsn_diff('$wal_now'::pg_lsn, '$wal0'::pg_lsn)::bigint")
    heap_pages=$(psql_as "$v" -Atc "SELECT pg_table_size('siu_table')/8192")
    idx_bytes=$(psql_as "$v" -Atc "SELECT pg_indexes_size('siu_table')")
    n_dead=$(psql_as "$v" -Atc "SELECT coalesce(n_dead_tup,0) FROM pg_stat_user_tables WHERE relname='siu_table'")

    local d_hot=$((now_hot - prev_hot))
    local d_tot=$((now_tot - prev_tot))
    local tps_i hot_pct
    if [ "$d_tot" -gt 0 ]; then
      tps_i=$(awk -v d="$d_tot" -v s="$SAMPLE" 'BEGIN{printf "%.1f", d/s}')
      hot_pct=$(awk -v h="$d_hot" -v t="$d_tot" 'BEGIN{printf "%.1f", 100*h/t}')
    else
      tps_i=0; hot_pct=0
    fi
    printf '%s,%d,%s,%s,%s,%s,%s,%s\n' "$v" "$t" "$tps_i" "$hot_pct" "$heap_pages" "$idx_bytes" "$wal_bytes" "$n_dead" >> "$OUT"
    printf '  %-6s t=%-5d tps=%8s hot=%-5s%% heap_pgs=%-7s idx=%-12s wal=%-12s dead=%s\n' \
      "$v" "$t" "$tps_i" "$hot_pct" "$heap_pages" "$idx_bytes" "$wal_bytes" "$n_dead"
    prev_hot=$now_hot
    prev_tot=$now_tot
  done

  wait "$pgb" 2>/dev/null || true
  stop_pg "$v"
}

for v in master tepid; do
  run_soak "$v"
done

echo "=== soak results: $OUT"
column -t -s, "$OUT" | head -80
