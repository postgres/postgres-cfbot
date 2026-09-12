#!/usr/bin/env bash
#
# bench_read_stream.sh - micro-benchmark for read-stream backward I/O combining.
#
# Uses the test_aio helper function:
#
#     read_stream_bench(rel regclass, blocks int4[], nruns int4) -> int8
#
# which streams a run of blocks forward (ascending) vs backward (descending)
# without materializing a result set, so the timing difference reflects the
# read-stream machinery itself.  For each io_combine_limit it prints the
# forward time, backward time and their ratio.
#
# The script initdb's a fresh cluster in DATADIR, starts a Postgres server on
# it, runs the benchmark and stops it again, using plain initdb/pg_ctl/psql
# binaries (from BINDIR if set, else PATH).
#
# Required:
#   DATADIR         data directory to create and run on (initdb'd if empty)
#
# Optional:
#   BINDIR          directory with pg_ctl/psql       (default: from PATH)
#   MODE            cached | cold | both             (default: both)
#   NBLOCKS         heap blocks in the test relation (default: 60000)
#   RUNS            timed repetitions per data point (default: 5)
#   NRUNS_CACHED    internal repetitions when cached (default: 20)
#   ICLS            io_combine_limit values to sweep (default: "1 8 16 32")
#   IO_METHOD       io_method GUC                    (default: worker)
#   SHARED_BUFFERS  shared_buffers GUC               (default: 1GB)
#   PGPORT          port to start the server on      (default: 5440)
#
# Example:
#   DATADIR=/path/to/pgdata BINDIR=/path/to/bin ./bench_read_stream.sh
#
set -euo pipefail

: "${DATADIR:?set DATADIR to a data directory to create and run on}"
[ -n "${BINDIR:-}" ] && export PATH="$BINDIR:$PATH"

MODE="${MODE:-both}"
NBLOCKS="${NBLOCKS:-60000}"
RUNS="${RUNS:-5}"
NRUNS_CACHED="${NRUNS_CACHED:-20}"
ICLS="${ICLS:-1 8 16 32}"
IO_METHOD="${IO_METHOD:-worker}"
SHARED_BUFFERS="${SHARED_BUFFERS:-1GB}"
export PGPORT="${PGPORT:-5440}"
export PGHOST=localhost
export PGDATABASE=postgres

REL="bench_rs"
FILLER_BYTES=6000
LOGFILE="$(mktemp)"
PSQL=(psql -X -q -v ON_ERROR_STOP=1)

psql_q() { "${PSQL[@]}" -c "$1" >/dev/null; }
scalar() { "${PSQL[@]}" -tAc "$1"; }
# psql prints one "Time:" line per statement; keep the last (the bench call).
time_ms() { awk '/^Time:/ { v=$2 } END { print v }'; }

stop_server() { pg_ctl -D "$DATADIR" -w -m fast stop >/dev/null 2>&1 || true; }
trap stop_server EXIT

[ -s "$DATADIR/PG_VERSION" ] || initdb -D "$DATADIR" -A trust >/dev/null

pg_ctl -D "$DATADIR" -l "$LOGFILE" -w \
	-o "-c port=$PGPORT -c listen_addresses=localhost \
	    -c io_method=$IO_METHOD -c shared_buffers=$SHARED_BUFFERS \
	    -c io_max_combine_limit=32 -c max_parallel_workers_per_gather=0" \
	start

psql_q "DROP EXTENSION IF EXISTS test_aio; CREATE EXTENSION test_aio;"
if [ "$(scalar "SELECT count(*) FROM pg_proc WHERE proname='read_stream_bench'")" != 1 ]; then
	echo "ERROR: read_stream_bench() not found; install a test_aio built with it." >&2
	exit 1
fi

# int4[] of 0..NBLOCKS-1, ascending (forward) or descending (backward).
arr_expr() {
	local order=ASC; [ "$1" = backward ] && order=DESC
	printf "(SELECT array_agg(g ORDER BY g %s)::int4[] FROM generate_series(0,%d) g)" \
		"$order" "$((NBLOCKS - 1))"
}

# Rebuild the test relation with one row per block, then update NBLOCKS to the
# actual page count.
psql_q "DROP TABLE IF EXISTS $REL;
	CREATE TABLE $REL (k int, filler text);
	ALTER TABLE $REL ALTER COLUMN filler SET STORAGE PLAIN;
	INSERT INTO $REL SELECT g, repeat('x', $FILLER_BYTES)
		FROM generate_series(1, $NBLOCKS) g;"
psql_q "VACUUM (ANALYZE, FREEZE) $REL;"
NBLOCKS="$(scalar "SELECT relpages FROM pg_class WHERE relname='$REL'")"

# $1 mode  $2 direction  $3 io_combine_limit -> best time in ms (echoed).
measure() {
	local mode="$1" dir="$2" icl="$3" arr nruns=1 best="" t
	arr="$(arr_expr "$dir")"
	[ "$mode" = cached ] && nruns="$NRUNS_CACHED"

	for _ in $(seq 1 "$RUNS"); do
		[ "$mode" = cold ] && psql_q "SELECT evict_rel('$REL');"
		t="$("${PSQL[@]}" -tA -c "\timing on" \
			-c "SET io_combine_limit=$icl;" \
			-c "SELECT read_stream_bench('$REL', $arr, $nruns);" | time_ms)"
		[ "$mode" = cached ] && t="$(awk "BEGIN{printf \"%.3f\", $t/$nruns}")"
		if [ -z "$best" ] || awk "BEGIN{exit !($t < $best)}"; then best="$t"; fi
	done
	echo "$best"
}

run_mode() {
	local mode="$1" icl fwd bwd
	printf '\n=== %s ===\n' "$mode"
	printf '%-16s | %12s | %12s | %8s\n' io_combine_limit forward'(ms)' backward'(ms)' bwd/fwd
	printf -- '-----------------+--------------+--------------+---------\n'
	for icl in $ICLS; do
		fwd="$(measure "$mode" forward  "$icl")"
		bwd="$(measure "$mode" backward "$icl")"
		printf '%-16s | %12s | %12s | %7sx\n' "$icl" "$fwd" "$bwd" \
			"$(awk "BEGIN{printf \"%.2f\", $bwd/$fwd}")"
	done
}

case "$MODE" in
	both)        MODES="cached cold" ;;
	cached|cold) MODES="$MODE" ;;
	*) echo "ERROR: MODE must be cached|cold|both" >&2; exit 1 ;;
esac
for m in $MODES; do run_mode "$m"; done
