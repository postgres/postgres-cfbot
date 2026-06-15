#!/usr/bin/env bash
# split_ab.sh: isolate the cost of the per-block VISIBILITYMAP_LOCATOR_SPLIT
# signal alone, separately from the selective-maintenance win that run.sh's
# other workloads measure.
#
# Both arms are the tepid tree; the only difference is whether BitmapAnd
# honors the split bit.  We build two variants from $REPO@$TEPID_REV:
#   split_on   the branch as is (heap advertises bitmap_and_inexact)
#   split_off  the same tree with heap's bitmap_and_inexact callback forced to
#              NULL, so BitmapAnd always intersects exactly
# so the delta is exactly the recheck/union the signal triggers, not the SIU
# feature itself.  Run bitmap_and_mixed.sql A/B alternating, ITERATIONS each,
# with a per-iteration pgbench --random-seed shared by both arms; report the
# median downstream from the collected rows.
set -euo pipefail

BENCH=${BENCH:-/scratch/siu-bench}
REPO=${REPO:-$HOME/ws/postgres/tepid}
TEPID_REV=${TEPID_REV:-tepid}
SCALE=${SCALE:-10}
CLIENTS=${CLIENTS:-16}
THREADS=${THREADS:-8}
DURATION=${DURATION:-60}
ITERATIONS=${ITERATIONS:-5}
SEED=${SEED:-}
PORT=${PORT:-57481}
SHARED_BUFFERS=${SHARED_BUFFERS:-4GB}
JOBS=${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 8 )}

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT=$BENCH/results/split_$TS.csv
LOGDIR=$BENCH/logs/split_$TS
SRCDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$LOGDIR" "$BENCH/results" "$BENCH/scripts"
[ "$SRCDIR" = "$BENCH/scripts" ] || cp "$SRCDIR"/*.sql "$BENCH/scripts/"
echo "iteration,variant,tps,latency_avg_ms" > "$OUT"
echo "=== split A/B run $TS -> $OUT (scale=$SCALE clients=$CLIENTS threads=$THREADS duration=${DURATION}s iterations=$ITERATIONS)"

bin_of() { echo "$BENCH/$1/usr/local/pgsql/bin"; }
LD_of() {
  local base=$BENCH/$1/usr/local/pgsql
  if [ -d "$base/lib64" ]; then echo "$base/lib64"; else echo "$base/lib"; fi
}
psql_as() { local v=$1; shift; LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/psql" -h /tmp -p "$PORT" -U postgres -X "$@"; }
pgbench_as() { local v=$1; shift; LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pgbench" -h /tmp -p "$PORT" -U postgres "$@"; }

# Build split_on (branch as is) and split_off (heap's split-bit read disabled).
build_split_variants() {
  cd "$REPO"
  if git status --porcelain | grep -v '^??' | grep -q .; then
    echo "split_ab: repo has uncommitted changes; stash or commit first" >&2; exit 1
  fi
  local orig; orig=$(git symbolic-ref --quiet --short HEAD || git rev-parse HEAD)
  trap "git checkout --quiet $orig" EXIT
  git checkout --quiet --detach "$TEPID_REV"

  _build() {  # $1 name, $2 = 1 to force the read-side hook off
    local name=$1 off=$2
    local prefix=$BENCH/$name bld=$BENCH/_build_$name
    echo "=== building $name into $prefix"
    rm -rf "$prefix" "$bld"; mkdir -p "$prefix"
    if [ "$off" = 1 ]; then
      # Force heap to advertise no split signal: BitmapAnd then always
      # intersects exactly, as it did before the feature.
      sed -i.bak 's/\.bitmap_and_inexact = heap_bitmap_and_inexact,/.bitmap_and_inexact = NULL,\/* split_ab: split-off *\//' \
        src/backend/access/heap/heapam_handler.c
    fi
    meson setup "$bld" --prefix="$prefix/usr/local/pgsql" \
      -Dbuildtype=release -Dcassert=false \
      "-Dextra_version=-split-$name" >/dev/null
    meson compile -C "$bld" -j "$JOBS"
    meson install -C "$bld" --destdir=/ >/dev/null
    if [ "$off" = 1 ]; then
      mv src/backend/access/heap/heapam_handler.c.bak src/backend/access/heap/heapam_handler.c
    fi
  }
  _build split_on 0
  _build split_off 1
}

start_pg() {
  local v=$1 datadir=$BENCH/_data_split_$v
  rm -rf "$datadir"; mkdir -p "$datadir"
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/initdb" -D "$datadir" -U postgres >"$LOGDIR/initdb_$v.log" 2>&1
  cat >> "$datadir/postgresql.conf" <<EOF
shared_buffers = $SHARED_BUFFERS
work_mem = 32MB
max_wal_size = 4GB
synchronous_commit = on
checkpoint_timeout = 10min
wal_level = replica
logging_collector = off
port = $PORT
EOF
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$datadir" \
    -o "-p $PORT" -l "$LOGDIR/pg_$v.log" start >/dev/null
  sleep 2
}
stop_pg() {
  local v=$1 datadir=$BENCH/_data_split_$v
  LD_LIBRARY_PATH="$(LD_of "$v")" "$(bin_of "$v")/pg_ctl" -D "$datadir" stop -m fast >/dev/null 2>&1 || true
}

seed() {
  local v=$1 rows=$((SCALE * 100000))
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

run_iter() {
  local iter=$1 v=$2 seed=${SEED:-$1}
  stop_pg "$v" || true
  start_pg "$v"
  seed "$v"
  local log="$LOGDIR/pgbench_${v}_iter${iter}.log"
  pgbench_as "$v" -n -f "$BENCH/scripts/bitmap_and_mixed.sql" \
    -c "$CLIENTS" -j "$THREADS" -T "$DURATION" -M prepared \
    --random-seed="$seed" -D scale="$SCALE" postgres >"$log" 2>&1 \
    || echo "  iter $iter $v: pgbench exited nonzero, see $log" >&2
  local tps lat
  tps=$(grep -oP 'tps = \K[0-9.]+' "$log" | tail -1)
  lat=$(grep -oP 'latency average = \K[0-9.]+' "$log" | tail -1)
  if [ -z "$tps" ] || [ -z "$lat" ]; then
    echo "  iter $iter  $v  FAILED (no tps/lat in $log); skipping CSV row" >&2
    stop_pg "$v"; return
  fi
  echo "$iter,$v,$tps,$lat" >> "$OUT"
  echo "  iter $iter  $v  tps=$tps  lat=${lat}ms"
  stop_pg "$v"
}

build_split_variants

# A/B alternate, not batched, to cancel drift.
for i in $(seq 1 "$ITERATIONS"); do
  run_iter "$i" split_off
  run_iter "$i" split_on
done

echo "=== done -> $OUT"
SUMMARY=${OUT%.csv}.summary.csv
awk -F, 'NR>1 {n[$2]++; v[$2","n[$2]]=$3}
  END{
    print "variant,runs,tps_median,tps_min,tps_max" > "/dev/stderr"
    for (k in n){c=n[k]; for(i=1;i<=c;i++)a[i]=v[k","i]
      for(i=1;i<=c;i++)for(j=i+1;j<=c;j++)if(a[j]<a[i]){t=a[i];a[i]=a[j];a[j]=t}
      med=(c%2)?a[(c+1)/2]:(a[c/2]+a[c/2+1])/2
      printf "%s,%d,%.1f,%.1f,%.1f\n", k, c, med, a[1], a[c] > "/dev/stderr"}
  }' "$OUT" 2> "$SUMMARY"
echo "=== summary (median TPS across $ITERATIONS iterations): $SUMMARY"
column -t -s, "$SUMMARY"
