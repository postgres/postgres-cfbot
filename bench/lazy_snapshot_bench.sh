#!/bin/bash
#
# Single benchmark run for "lazy snapshot distribution" patch.
#
# Simulates: one long-running transaction (with K inserts) coexists with N
# concurrent catalog-modifying commits, then drains via pg_logical_slot_get_changes.
#
# Output (CSV row to stdout):
#   label,N,K,ldwm,decoding_ms,decoded_changes,spill_txns,spill_count,spill_bytes,total_bytes,ddl_loop_sec
#
# Assumes a running PG cluster with wal_level=logical.  To compare master vs
# patch, point this script at two different clusters built from each binary.

set -euo pipefail

# -------- args --------
N=""
K="1"
LABEL=""
LDWM="64kB"
SLOT="lazy_bench"
VERBOSE=0

usage() {
  cat <<EOF
Usage: $0 -N <num_ddl_commits> -l <label> [-K <inserts>] [-m <ldwm>] [-s <slot>] [-v]

Required:
  -N <N>      number of concurrent CREATE/DROP TABLE pairs during long txn
  -l <label>  CSV tag (e.g. "master" or "patch")

Options:
  -K <K>      inserts in long txn (default 1)
  -m <mem>   logical_decoding_work_mem (default 64kB; small to amplify spill)
  -s <slot>  slot name (default lazy_bench)
  -v          verbose

PG connection: standard libpq env vars (PGHOST/PGPORT/PGDATABASE/PGUSER).
EOF
  exit 1
}

while getopts "N:K:l:m:s:vh" opt; do
  case $opt in
    N) N="$OPTARG" ;;
    K) K="$OPTARG" ;;
    l) LABEL="$OPTARG" ;;
    m) LDWM="$OPTARG" ;;
    s) SLOT="$OPTARG" ;;
    v) VERBOSE=1 ;;
    h|*) usage ;;
  esac
done

[[ -z "$N" || -z "$LABEL" ]] && usage

log() { [[ $VERBOSE -eq 1 ]] && echo "[bench] $*" >&2 || true; }

PSQL="psql -X -At -q"

# Quote string for SQL.
sqlstr() { printf "'%s'" "${1//\'/\'\'}"; }

# -------- preflight --------

# Verify cluster reachable and wal_level=logical.
wal_level=$($PSQL -c "SHOW wal_level;" || { echo "cannot connect to PG" >&2; exit 1; })
if [[ "$wal_level" != "logical" ]]; then
  echo "wal_level must be 'logical' (got '$wal_level')" >&2
  echo "set: ALTER SYSTEM SET wal_level = logical; then restart" >&2
  exit 1
fi

# Set logical_decoding_work_mem for this session via slot creation parameter
# is not possible; it's a backend GUC. We rely on session-level SET inside the
# decoding query below.

# -------- setup --------
log "setup: drop old slot/tables, create fresh slot"
$PSQL <<SQL >/dev/null
SELECT pg_drop_replication_slot($(sqlstr "$SLOT"))
  WHERE EXISTS (SELECT 1 FROM pg_replication_slots
                WHERE slot_name = $(sqlstr "$SLOT"));
DROP TABLE IF EXISTS bench_data;
CREATE TABLE bench_data (i int);
SELECT pg_create_logical_replication_slot($(sqlstr "$SLOT"), 'test_decoding');
SELECT pg_stat_reset_replication_slot($(sqlstr "$SLOT"));
SQL

# -------- long-running txn (background) --------
# It does K inserts then sleeps long enough for the DDL loop to finish.
# Generous sleep: 0.05s per DDL + 10s buffer. We don't poll; if DDLs finish
# early, the long txn just sits idle (harmless for the measurement).
# Sleep budget covers DDL loop time + safety margin.  Tuned for ~1000 DDL/s
# in single-session mode; conservative 10x factor for slow CI machines.
SLEEP_SEC=$(awk "BEGIN { printf \"%.2f\", $N * 0.01 + 5 }")
log "long txn sleep budget: ${SLEEP_SEC}s"

(
  $PSQL <<SQL
BEGIN;
INSERT INTO bench_data SELECT generate_series(1, $K);
SELECT pg_sleep($SLEEP_SEC);
COMMIT;
SQL
) &
LONG_PID=$!

# Wait for the long txn to actually start its INSERT before kicking off DDLs.
sleep 1

# -------- concurrent DDL loop --------
# Single psql session, one statement per line.  Each unwrapped CREATE/DROP
# auto-commits as its own transaction, so this generates N catalog-modifying
# commits in WAL, identical to N separate psql -c calls but ~100x faster.
log "running $N CREATE/DROP TABLE pairs in a single psql session"
DDL_START=$(date +%s.%N)
{
  echo '\set ON_ERROR_STOP on'
  for i in $(seq 1 "$N"); do
    printf 'CREATE TABLE bench_t%d (a int); DROP TABLE bench_t%d;\n' "$i" "$i"
  done
} | $PSQL -f - >/dev/null
DDL_END=$(date +%s.%N)
DDL_LOOP_SEC=$(awk "BEGIN { printf \"%.2f\", $DDL_END - $DDL_START }")
log "DDL loop done in ${DDL_LOOP_SEC}s"

# Wait for long txn to commit.
wait "$LONG_PID"
log "long txn committed"

# -------- decode + measure --------
log "draining slot via pg_logical_slot_get_changes"
DECODE_START=$(date +%s.%N)
DECODED=$($PSQL <<SQL
SET logical_decoding_work_mem = '$LDWM';
SELECT count(*) FROM pg_logical_slot_get_changes(
  $(sqlstr "$SLOT"), NULL, NULL,
  'include-xids', '0', 'skip-empty-xacts', '1'
);
SQL
)
DECODE_END=$(date +%s.%N)
DECODE_MS=$(awk "BEGIN { printf \"%.0f\", ($DECODE_END - $DECODE_START) * 1000 }")
log "decoded $DECODED changes in ${DECODE_MS}ms"

# Capture stats (the slot's lifetime stats; we reset at setup, so this is just
# the current decoding run).
STATS=$($PSQL -c "
SELECT spill_txns || ',' || spill_count || ',' || spill_bytes || ',' || total_bytes
FROM pg_stat_replication_slots
WHERE slot_name = $(sqlstr "$SLOT");")

# -------- cleanup --------
$PSQL <<SQL >/dev/null
SELECT pg_drop_replication_slot($(sqlstr "$SLOT"));
DROP TABLE bench_data;
SQL

# -------- output --------
echo "$LABEL,$N,$K,$LDWM,$DECODE_MS,$DECODED,$STATS,$DDL_LOOP_SEC"
