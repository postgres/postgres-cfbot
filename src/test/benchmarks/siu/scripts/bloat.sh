#!/usr/bin/env bash
# Single-variant bloat benchmark for HOT-indexed (SIU) updates.
#
# The A/B run.sh measures TPS/WAL/aggregate size; it does not isolate two
# bloat properties of the HOT-indexed path, which this script demonstrates:
#
#   1. Stale index entries on the CHANGED index accumulate between vacuums,
#      but VACUUM reclaims them, so size stays bounded across update+vacuum
#      cycles.  Skipping vacuum lets them grow unbounded (the inherent
#      inter-vacuum bloat; read-filtered by the crossed-attribute bitmap meanwhile).
#   2. An index on an UNCHANGED column is skipped by HOT-indexed updates (the
#      selective-update benefit), visible as a skip count.  Updates that fall
#      back to non-HOT (e.g. when the page has no room for the chain) still
#      insert into it, so its size reflects only the non-HOT remainder.
#
# Uses the tepid variant built by build.sh (override BINDIR for any build) and
# a throwaway cluster under $BENCH, so it never touches the A/B pgdata.
#
# Env: BENCH (default /scratch/siu-bench), BINDIR (default tepid variant bin),
#      PORT (default 57481), ROWS (default 5000), CYCLES (default 8),
#      UPDATES (updates per row per cycle, default 20).
set -euo pipefail

BENCH=${BENCH:-/scratch/siu-bench}
BINDIR=${BINDIR:-$BENCH/tepid/usr/local/pgsql/bin}
PORT=${PORT:-57481}
ROWS=${ROWS:-5000}
CYCLES=${CYCLES:-8}
UPDATES=${UPDATES:-20}
DATADIR=$BENCH/_data_bloat

base=$(dirname "$BINDIR")
if [ -d "$base/lib64" ]; then
  export LD_LIBRARY_PATH="$base/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$base/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

PSQL=("$BINDIR/psql" -h /tmp -p "$PORT" -U postgres -X -q)
P() { "${PSQL[@]}" -At "$@"; }

"$BINDIR/pg_ctl" -D "$DATADIR" stop -m fast >/dev/null 2>&1 || true
rm -rf "$DATADIR"
"$BINDIR/initdb" -D "$DATADIR" -U postgres --no-sync >/dev/null
cat >> "$DATADIR/postgresql.conf" <<EOF
port = $PORT
autovacuum = off
fsync = off
EOF
"$BINDIR/pg_ctl" -D "$DATADIR" -o "-p $PORT" -l "$BENCH/_bloat.log" -w start >/dev/null
trap '"$BINDIR/pg_ctl" -D "$DATADIR" stop -m fast >/dev/null 2>&1 || true' EXIT
"${PSQL[@]}" -c "CREATE EXTENSION IF NOT EXISTS pgstattuple;" >/dev/null

# $1 = table name, $2 = vacuum_each (1/0).  Returns final idx_a composition.
run_arm() {
  local tbl=$1 vac=$2
  "${PSQL[@]}" <<SQL >/dev/null
DROP TABLE IF EXISTS $tbl;
CREATE TABLE $tbl (id int PRIMARY KEY, a int, b int, pad text) WITH (fillfactor = 50);
CREATE INDEX ${tbl}_a ON $tbl(a);   -- changed column
CREATE INDEX ${tbl}_b ON $tbl(b);   -- never changed
INSERT INTO $tbl SELECT g, g, g, repeat('x', 40) FROM generate_series(1, $ROWS) g;
VACUUM (FREEZE, ANALYZE) $tbl;
SQL
  local cyc
  for ((cyc = 1; cyc <= CYCLES; cyc++)); do
    "${PSQL[@]}" -c "DO \$\$ BEGIN FOR u IN 1..$UPDATES LOOP UPDATE $tbl SET a = a + 1; END LOOP; END \$\$;" >/dev/null
    [ "$vac" = 1 ] && "${PSQL[@]}" -c "VACUUM $tbl;" >/dev/null
  done
  P -c "SELECT '$tbl(vacuum_each=$vac):'
        || ' idx_a_kb=' || pg_relation_size('${tbl}_a')/1024
        || ' idx_a_live=' || (SELECT tuple_count FROM pgstattuple('${tbl}_a'))
        || ' idx_a_free%=' || round((SELECT free_percent FROM pgstattuple('${tbl}_a'))::numeric,1)
        || ' idx_b_kb=' || pg_relation_size('${tbl}_b')/1024
        || ' idx_b_skips=' || coalesce((SELECT n_tup_hot_indexed_upd_skipped
                                        FROM pg_stat_all_indexes WHERE indexrelname='${tbl}_b'), 0);"
}

echo "=== HOT-indexed bloat: $CYCLES cycles x ($UPDATES updates/row x $ROWS rows) ==="
run_arm t_vac 1
run_arm t_novac 0
echo "idx_a: bounded with vacuum_each=1, unbounded with =0 (stale entries accumulate"
echo "until reclaimed).  idx_b_skips counts entries the HOT-indexed path avoided on"
echo "the unchanged index; idx_b_kb is only the non-HOT-fallback remainder."
