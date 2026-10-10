#!/usr/bin/env bash
# A/B pgbench harness for tepid: master (upstream) vs tepid (HOT-indexed).
#
# Env vars:
#   SCALE       pgbench -s (also multiplier for siu_table row count = SCALE*100k)
#   CLIENTS     pgbench -c
#   THREADS     pgbench -j
#   DURATION    pgbench -T (seconds per workload)
#   WIDE_COLS   # of indexed columns in the wide_table (default 16)
#   WIDE_STEPS  comma-separated list of "updated columns" counts for
#                 the wide workload (default "0,1,4,8,WIDE_COLS")
#   PORT        postgres port (default 57480)
#
# For each variant in {master, tepid}:
#   initdb fresh pgdata, start postgres, create test objects,
#   run workloads (pgbench -N simple_update, hot_indexed_update, hot_indexed_mixed,
#   read_indexscan, and wide_N for each value in WIDE_STEPS), collect TPS + HOT
#   counts + WAL delta + peak CPU/RSS sampled from ps.
#
# The variants alternate at every iteration (ABAB..., not AAAA then BBBB) so
# thermal drift and background noise fall on both equally, and each iteration
# uses one pgbench --random-seed shared by both arms so they see the same
# workload.  Report the per-workload median across iterations, never a single
# run; a run-to-run spread of a few percent is normal (see the pgsql-hackers
# threads on pgbench variance).  Set AA=1 for an A/A control that runs the
# master build as both arms, to measure that noise floor before trusting an
# A/B delta.
# Emits a per-run CSV plus a median/min/max/spread summary under $BENCH/results/.
set -euo pipefail

BENCH=${BENCH:-/scratch/siu-bench}
SCALE=${SCALE:-20}
CLIENTS=${CLIENTS:-16}
THREADS=${THREADS:-8}
DURATION=${DURATION:-120}
WIDE_COLS=${WIDE_COLS:-16}
WIDE_STEPS=${WIDE_STEPS:-0,1,4,8,16}
PORT=${PORT:-57480}
ITERATIONS=${ITERATIONS:-5}
SEED=${SEED:-}            # base pgbench --random-seed; per-iteration if unset
AA=${AA:-0}              # 1: A/A control (master as both arms)

# Make the workload scripts available at the path the driver references,
# whether or not $BENCH is this checkout.
SRCDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$BENCH/scripts"
if [ "$SRCDIR" != "$BENCH/scripts" ]; then
  cp "$SRCDIR"/*.sql "$BENCH/scripts/"
fi

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$BENCH/results/$TS.csv
LOGDIR=$BENCH/logs/$TS
mkdir -p "$LOGDIR" "$BENCH/results"
echo "iteration,variant,workload,tps,latency_avg_ms,classic_hot_updates,hot_indexed_updates,non_hot_updates,total_updates,wal_bytes,bloat_pages_before,bloat_pages_after,index_size_before,index_size_after,cpu_pct_peak,rss_mib_peak,per_index_before,per_index_after" > "$OUT"
echo "=== siu-bench A/B run $TS -> $OUT (scale=$SCALE clients=$CLIENTS threads=$THREADS duration=${DURATION}s)"

bin_of() {
  echo "$BENCH/$1/usr/local/pgsql/bin"
}

LD_of() {
  local base=$BENCH/$1/usr/local/pgsql
  # Linux distros that split 64-bit libs use lib64; most others use lib.
  if [ -d "$base/lib64" ]; then
    echo "$base/lib64"
  else
    echo "$base/lib"
  fi
}

psql_as() {
  local v=$1; shift
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/psql" -h /tmp -p "$PORT" -U postgres -X "$@"
}

pgbench_as() {
  local v=$1; shift
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pgbench" -h /tmp -p "$PORT" -U postgres "$@"
}

start_pg() {
  local v=$1
  local datadir=$BENCH/_data_$v
  [ -d "$datadir" ] && find "$datadir" -mindepth 1 -delete && rmdir "$datadir"
  mkdir -p "$datadir"

  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/initdb" -D "$datadir" -U postgres >"$LOGDIR/initdb_$v.log" 2>&1
  local sb=${SHARED_BUFFERS:-512MB}
  cat >> "$datadir/postgresql.conf" <<EOF
shared_buffers = $sb
work_mem = 32MB
max_wal_size = 4GB
synchronous_commit = on
checkpoint_timeout = 10min
wal_level = replica
log_destination = 'stderr'
logging_collector = off
port = $PORT
EOF
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$datadir" \
    -o "-p $PORT" -l "$LOGDIR/pg_$v.log" start >/dev/null
  sleep 2
}

stop_pg() {
  local v=$1
  local datadir=$BENCH/_data_$v
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$datadir" stop -m fast >/dev/null 2>&1 || true
}

postmaster_pid() {
  local v=$1
  head -1 "$BENCH/_data_$v/postmaster.pid" 2>/dev/null
}

setup_schemas() {
  local v=$1
  seed_siu_table "$v"
  seed_wide_table "$v"
  # pgbench schema for built-in simple_update.
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pgbench" -h /tmp -p "$PORT" -U postgres \
    -i -s "$SCALE" -q postgres >"$LOGDIR/pgbench_init_$v.log" 2>&1
}

# seed_siu_table: (re)create the narrow table used by the siu_* workloads.
seed_siu_table() {
  local v=$1
  local rows=$((SCALE * 100000))
  psql_as "$v" <<SQL
DROP TABLE IF EXISTS siu_table;
CREATE TABLE siu_table(a int PRIMARY KEY, b int, c int, d int, e text);
CREATE INDEX siu_b ON siu_table(b);
CREATE INDEX siu_c ON siu_table(c);
CREATE INDEX siu_d ON siu_table(d);
INSERT INTO siu_table
  SELECT i, i, i, i, repeat('x', 20) FROM generate_series(1, $rows) AS i;
VACUUM (FULL, ANALYZE) siu_table;
CHECKPOINT;
SQL
}

# seed_wide_table: (re)create the wide table with WIDE_COLS indexed columns.
seed_wide_table() {
  local v=$1
  local coldefs="" insertcols="" insertvals="" idxlist=""
  for i in $(seq 1 "$WIDE_COLS"); do
    coldefs+=", c$i int"
    insertcols+=", c$i"
    insertvals+=", i"
    idxlist+="CREATE INDEX wide_c$i ON wide_table(c$i); "
  done
  local wide_rows=$((SCALE * 1000))
  psql_as "$v" <<SQL
DROP TABLE IF EXISTS wide_table;
CREATE TABLE wide_table(id int PRIMARY KEY $coldefs);
$idxlist
INSERT INTO wide_table(id $insertcols) SELECT i $insertvals FROM generate_series(1, $wide_rows) AS i;
VACUUM (FULL, ANALYZE) wide_table;
CHECKPOINT;
SQL
}

# reset_state: restore a workload's target table to its seeded baseline.
# Used between workloads so per-workload bloat/idx_size deltas are not
# polluted by carryover from earlier workloads in the same variant run.
# For pgbench_accounts we re-initialise via `pgbench -i`; for our
# hand-rolled tables we drop + recreate + reseed.
reset_state() {
  local v=$1 table=$2
  case "$table" in
    pgbench_accounts)
      LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pgbench" -h /tmp -p "$PORT" -U postgres \
        -i -s "$SCALE" -q postgres >>"$LOGDIR/pgbench_init_$v.log" 2>&1
      psql_as "$v" -c "CHECKPOINT" >/dev/null
      ;;
    siu_table)
      seed_siu_table "$v"
      ;;
    wide_table)
      seed_wide_table "$v"
      ;;
    *)
      echo "reset_state: unknown table $table" >&2
      return 1
      ;;
  esac
  psql_as "$v" -c "SELECT pg_stat_reset_single_table_counters('$table'::regclass::oid)" >/dev/null
}

bloat_stats() {
  local v=$1 table=$2
  psql_as "$v" -Atc "SELECT pg_table_size('$table')/8192 || ',' || pg_indexes_size('$table')"
}

# siu_count: number of HOT-indexed updates observed on $table since its
# pgstat counters were last reset.  Returns "0" on master (where the
# counter column does not exist) so the CSV column stays numeric.
siu_count() {
  local v=$1 table=$2
  local val
  val=$(psql_as "$v" -Atc \
    "SELECT coalesce(n_tup_hot_indexed_upd, 0) FROM pg_stat_user_tables WHERE relname='$table'" 2>/dev/null)
  [[ "$val" =~ ^[0-9]+$ ]] || val=0
  echo "$val"
}

# per_index_sizes: emit "idx1=bytes;idx2=bytes;..." for the indexes on
# $table, sorted by indexrelid.  Used by the wide_* workloads so we can
# see per-column index growth rather than just the aggregate.  Returns
# the literal "none" when $table has no indexes.
per_index_sizes() {
  local v=$1 table=$2
  local out
  out=$(psql_as "$v" -Atc "SELECT string_agg(
           i.relname || '=' || pg_relation_size(i.oid)::text,
           ';' ORDER BY i.oid)
         FROM pg_class t
         JOIN pg_index ix ON ix.indrelid = t.oid
         JOIN pg_class i  ON i.oid = ix.indexrelid
         WHERE t.relname = '$table'")
  [ -n "$out" ] || out="none"
  echo "$out"
}

sample_peak() {
  # Sample CPU / RSS of the postmaster tree for $DURATION+5 seconds.
  # Writes "peak_cpu_pct,peak_rss_mib" to the given outfile.  Portable across
  # Linux / FreeBSD (falls back to pgrep + per-pid ps where --ppid isn't
  # available).  Returns 'NA,NA' if the sampler can't collect useful data.
  local outfile=$1 v=$2
  local leader
  leader=$(postmaster_pid "$v")
  [ -z "$leader" ] && { echo "NA,NA" > "$outfile"; return; }
  local dur=$(( DURATION + 5 ))
  (
    local max_cpu=0
    local max_rss=0
    local t0=$(date +%s)
    while :; do
      # Children of the leader + the leader itself.
      local pids
      pids=$( (pgrep -P "$leader" 2>/dev/null; echo "$leader") | tr '\n' ' ')
      local sample
      sample=$(ps -o pcpu=,rss= -p $pids 2>/dev/null | \
               awk '{cpu+=$1; rss+=$2} END{printf "%.1f %d\n", cpu+0, rss+0}')
      local c r
      read -r c r <<<"$sample"
      if [ -n "${c:-}" ] && [ -n "${r:-}" ]; then
        awk -v m="$max_cpu" -v c="$c" 'BEGIN{exit !(c>m)}' && max_cpu=$c
        [ "$r" -gt "$max_rss" ] 2>/dev/null && max_rss=$r
      fi
      local now=$(date +%s)
      [ $((now - t0)) -ge "$dur" ] && break
      sleep 1
    done
    local rss_mib=$(( max_rss / 1024 ))
    echo "$max_cpu,$rss_mib" > "$outfile"
  ) &
  echo $!
}

run_one() {
  local iter=$1 v=$2 workload=$3 script=$4 table=${5:-siu_table} extra_set=${6:-}
  local seed=${SEED:-$iter}
  local seedopt="--random-seed=$seed" 

  local wal_start wal_end hot_start hot_end total_start total_end tps lat
  local siu_start siu_end
  local bloat_before bloat_after idx_before idx_after
  local per_idx_before per_idx_after
  read -r bloat_before idx_before <<<"$(bloat_stats "$v" "$table" | tr , ' ')"
  per_idx_before=$(per_index_sizes "$v" "$table")

  wal_start=$(psql_as "$v" -Atc "SELECT pg_current_wal_lsn()::text")
  hot_start=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_hot_upd,0) FROM pg_stat_user_tables WHERE relname='$table'")
  siu_start=$(siu_count "$v" "$table")
  total_start=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_upd,0) FROM pg_stat_user_tables WHERE relname='$table'")

  local out="$LOGDIR/${v}_${workload}_i${iter}.log"
  local cpu_rss_file=$LOGDIR/${v}_${workload}_i${iter}.cpu
  local sampler_pid
  sampler_pid=$(sample_peak "$cpu_rss_file" "$v")

  set +e
  case "$workload" in
    simple_update)
      pgbench_as "$v" -N -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        $seedopt -n postgres >"$out" 2>&1
      ;;
    wide_*)
      # build the SET clause from extra_set which is "c1=:v,c2=:v,..."
      pgbench_as "$v" -f <(sed "s/:wide_set_clause/$extra_set/" "$script") \
        -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        $seedopt -D "scale=$SCALE" -n postgres >"$out" 2>&1
      ;;
    read_indexscan)
      # read-only; pass the row count so the script can pick random keys
      pgbench_as "$v" -f "$script" -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        $seedopt -D "rows=$((SCALE * 100000))" -n postgres >"$out" 2>&1
      ;;
    *)
      pgbench_as "$v" -f "$script" -c "$CLIENTS" -j "$THREADS" -T "$DURATION" \
        $seedopt -n postgres >"$out" 2>&1
      ;;
  esac
  set -e

  wait "$sampler_pid" 2>/dev/null || true
  local cpu_rss
  cpu_rss=$(cat "$cpu_rss_file" 2>/dev/null || echo "NA,NA")

  tps=$(awk '/tps = /{print $3; exit}' "$out")
  lat=$(awk '/latency average = /{print $4; exit}' "$out")
  tps=${tps:-NA}
  lat=${lat:-NA}

  wal_end=$(psql_as "$v" -Atc "SELECT pg_current_wal_lsn()::text")
  hot_end=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_hot_upd,0) FROM pg_stat_user_tables WHERE relname='$table'")
  siu_end=$(siu_count "$v" "$table")
  total_end=$(psql_as "$v" -Atc "SELECT coalesce(n_tup_upd,0) FROM pg_stat_user_tables WHERE relname='$table'")

  local wal_bytes
  wal_bytes=$(psql_as "$v" -Atc "SELECT pg_wal_lsn_diff('$wal_end'::pg_lsn, '$wal_start'::pg_lsn)::bigint")

  # Capture a WAL record-type histogram for this workload.  pg_waldump's
  # --stats=record output is rich (~60 lines) so stash it in LOGDIR
  # rather than trying to fold into the CSV.  Tolerate failures: if the
  # segment containing wal_start has been recycled (rare with
  # max_wal_size=4GB but possible under long chained runs), we emit a
  # note and move on instead of aborting the whole run.
  local wal_stats_file=$LOGDIR/${v}_${workload}_i${iter}.walstats
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_waldump" \
    --stats=record -p "$BENCH/_data_$v/pg_wal" \
    --start="$wal_start" --end="$wal_end" \
    > "$wal_stats_file" 2> "${wal_stats_file}.err" \
    || echo "pg_waldump unavailable for this range; see ${wal_stats_file}.err" > "$wal_stats_file"

  read -r bloat_after idx_after <<<"$(bloat_stats "$v" "$table" | tr , ' ')"
  per_idx_after=$(per_index_sizes "$v" "$table")

  local hot=$((hot_end - hot_start))
  local siu=$((siu_end - siu_start))
  local tot=$((total_end - total_start))
  local classic_hot=$((hot - siu))
  local non_hot=$((tot - hot))

  printf '%s,%s,%s,%s,%s,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$iter" "$v" "$workload" "$tps" "$lat" "$classic_hot" "$siu" "$non_hot" "$tot" \
    "$wal_bytes" \
    "$bloat_before" "$bloat_after" \
    "$idx_before" "$idx_after" \
    "$cpu_rss" "$per_idx_before" "$per_idx_after" >> "$OUT"
  printf '  %-8s %-14s tps=%10s lat=%6s classic_hot=%7d hi=%7d non_hot=%7d tot=%-7d wal=%12s bloat=%s->%s idx=%s->%s cpu_rss=%s\n' \
    "$v" "$workload" "$tps" "$lat" "$classic_hot" "$siu" "$non_hot" "$tot" "$wal_bytes" \
    "$bloat_before" "$bloat_after" "$idx_before" "$idx_after" "$cpu_rss"
}

build_wide_set_clause() {
  # emit e.g. "c1=:v,c2=:v,...,cN=:v" for first N cols.
  local n=$1
  if [ "$n" -eq 0 ]; then
    # No indexed-col update; touch a non-indexed column (id % 1 so it's a no-op)
    echo "id=id"
    return
  fi
  local clauses=""
  for i in $(seq 1 "$n"); do
    [ -n "$clauses" ] && clauses+=","
    clauses+="c$i=:v"
  done
  echo "$clauses"
}

# In A/A mode both arms are the master build; the second is labelled "tepid"
# so the summary still pairs them, and any delta is pure noise.
arm_build() { if [ "$AA" = 1 ]; then echo master; else echo "$1"; fi; }

for iter in $(seq 1 "$ITERATIONS"); do
  aa_note=""; [ "$AA" = 1 ] && aa_note=", A/A control"
  echo "=== iteration $iter/$ITERATIONS (seed=${SEED:-$iter}$aa_note)"
  for v in master tepid; do
    b=$(arm_build "$v")
    echo "--- iteration $iter variant $v (build $b)"
    stop_pg "$b" || true
    start_pg "$b"
    setup_schemas "$b"

    run_one "$iter" "$v" simple_update ''                 pgbench_accounts
    reset_state "$b" siu_table
    run_one "$iter" "$v" hot_indexed_update "$BENCH/scripts/hot_indexed_update.sql" siu_table
    reset_state "$b" siu_table
    run_one "$iter" "$v" hot_indexed_mixed  "$BENCH/scripts/hot_indexed_mixed.sql"  siu_table
    reset_state "$b" siu_table
    run_one "$iter" "$v" read_indexscan     "$BENCH/scripts/read_indexscan.sql"     siu_table

    for n in ${WIDE_STEPS//,/ }; do
      reset_state "$b" wide_table
      run_one "$iter" "$v" "wide_${n}" "$BENCH/scripts/wide_update.sql" wide_table \
              "$(build_wide_set_clause "$n")"
    done

    stop_pg "$b"
  done
done

echo "=== per-run results: $OUT"
column -t -s, "$OUT" | head -60

# Median / min / max / spread of TPS per (workload, variant) across iterations.
SUMMARY=${OUT%.csv}.summary.csv
awk -F, 'NR>1 && $4!="NA" {k=$3 SUBSEP $2; n[k]++; v[k","n[k]]=$4}
  END{
    print "workload,variant,runs,tps_median,tps_min,tps_max,spread_pct" > "/dev/stderr"
    for (k in n) {
      c=n[k]; for(i=1;i<=c;i++) a[i]=v[k","i]
      for(i=1;i<=c;i++)for(j=i+1;j<=c;j++)if(a[j]<a[i]){t=a[i];a[i]=a[j];a[j]=t}
      med=(c%2)?a[(c+1)/2]:(a[c/2]+a[c/2+1])/2
      split(k,kk,SUBSEP)
      sp=(a[c]>0)?(a[c]-a[1])/med*100:0
      printf "%s,%s,%d,%.1f,%.1f,%.1f,%.1f\n", kk[1], kk[2], c, med, a[1], a[c], sp > "/dev/stderr"
    }
  }' "$OUT" 2> "$SUMMARY"
echo "=== summary (median TPS across $ITERATIONS iterations): $SUMMARY"
column -t -s, "$SUMMARY"
