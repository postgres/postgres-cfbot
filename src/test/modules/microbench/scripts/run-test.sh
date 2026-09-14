#!/usr/bin/env bash
#
# Run one microbench test folder.
#
# Usage: run-test.sh TEST
# Env:   TOP_BUILDDIR, PG_CONFIG, MICROBENCH_PORT (default 55432),
#        MICROBENCH_PARAMS
#
set -euo pipefail

TEST=${1:?usage: run-test.sh TEST}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
TOP_BUILDDIR=${TOP_BUILDDIR:-$(cd "$MODULE_DIR/../../../.." && pwd)}
PORT=${MICROBENCH_PORT:-55432}
LOGDIR="$MODULE_DIR/.tmp_check/log"
DATADIR="$MODULE_DIR/.tmp_check/data"
ROUNDS=${MICROBENCH_ROUNDS:-1000}
MICROBENCH_PARAMS=${MIBROBENCH_PARAMS:""}
log() { printf '%s\n' "$*" >&2; }

test -f "$MODULE_DIR/$TEST/query.sql" || {
	log "missing $MODULE_DIR/$TEST/query.sql"
	exit 1
}
pick_pg_config() {
	if [[ -n "${PG_CONFIG:-}" && -x "$PG_CONFIG" ]]; then
		printf '%s\n' "$PG_CONFIG"
		return
	fi

	local makefile_global="$TOP_BUILDDIR/src/Makefile.global"
	if [[ -f "$makefile_global" ]]; then
		local prefix candidate

		prefix=$(sed -n 's/^prefix := //p' "$makefile_global" | head -1)
		if [[ -n "$prefix" ]]; then
			candidate="$prefix/bin/pg_config"
			if [[ -x "$candidate" ]]; then
				printf '%s\n' "$candidate"
				return
			fi
			log "configured prefix $prefix has no executable pg_config at $candidate"
		fi
	fi

	return 1
}

PG_CONFIG=$(pick_pg_config) || {
	log "no PostgreSQL install found; set PG_CONFIG or run configure && make install at repo root"
	exit 1
}

BINDIR=$("$PG_CONFIG" --bindir)
LIBDIR=$("$PG_CONFIG" --libdir)
log "==> using $($PG_CONFIG --version) [$BINDIR]"

log "==> building and installing microbench..."
make -C "$MODULE_DIR" MICROBENCH_POSTGRES="$BINDIR/postgres" install

export PATH="$BINDIR:$PATH"
case "$(uname -s)" in
	Darwin) export DYLD_LIBRARY_PATH="$LIBDIR:${DYLD_LIBRARY_PATH:-}" ;;
	*) export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}" ;;
esac

mkdir -p "$LOGDIR"

postgres_build_id() {
	# Re-init when the installed postgres binary changes (catalog bumps, rebuilds).
	printf '%s:%s' "$("$BINDIR/postgres" --version)" \
		"$(shasum -a 256 "$BINDIR/postgres" | awk '{print $1}')"
}

ensure_datadir() {
	local build_id stamp_file="$DATADIR/.microbench_build_id"

	build_id=$(postgres_build_id)
	if [[ -f "$DATADIR/PG_VERSION" && -f "$stamp_file" && "$(cat "$stamp_file")" == "$build_id" ]]; then
		return 0
	fi

	if [[ -f "$DATADIR/PG_VERSION" ]]; then
		log "==> stale datadir (postgres rebuilt); re-initdb..."
	else
		log "==> initdb..."
	fi

	rm -rf "$DATADIR"
	"$BINDIR/initdb" -D "$DATADIR" --auth trust --no-sync --no-instructions -N \
		>"$LOGDIR/initdb.log" 2>&1
	printf '%s\n' "$build_id" > "$stamp_file"
}

ensure_datadir

cleanup() {
	if "$BINDIR/pg_ctl" -D "$DATADIR" status >/dev/null 2>&1; then
		"$BINDIR/pg_ctl" -D "$DATADIR" stop -m fast >>"$LOGDIR/pg_ctl.log" 2>&1 || true
	fi
}
trap cleanup EXIT

if ! "$BINDIR/pg_ctl" -D "$DATADIR" status >/dev/null 2>&1; then
	log "==> starting postgres on port $PORT..."
	if ! "$BINDIR/pg_ctl" -D "$DATADIR" -l "$LOGDIR/postgres.log" \
		-o "-p $PORT -F -h '' -c shared_buffers=128MB -c max_worker_processes=256 -c max_parallel_workers=256" start \
		>>"$LOGDIR/pg_ctl.log" 2>&1; then
		if grep -q 'incompatible with server' "$LOGDIR/postgres.log"; then
			log "==> postgres rejected datadir; re-initdb..."
			rm -rf "$DATADIR"
			ensure_datadir
			"$BINDIR/pg_ctl" -D "$DATADIR" -l "$LOGDIR/postgres.log" \
				-o "-p $PORT -F -h '' -c shared_buffers=128MB -c max_worker_processes=256 -c max_parallel_workers=256" start \
				>>"$LOGDIR/pg_ctl.log" 2>&1
		else
			log "pg_ctl start failed; see $LOGDIR/postgres.log and $LOGDIR/pg_ctl.log"
			exit 1
		fi
	fi
fi

log "==> CREATE EXTENSION microbench"
: >"$LOGDIR/psql.log"
"$BINDIR/psql" -v ON_ERROR_STOP=1 -p "$PORT" -d postgres \
	-c "DROP EXTENSION IF EXISTS microbench CASCADE; CREATE EXTENSION microbench;" \
	>>"$LOGDIR/psql.log" 2>&1

log "==> running $TEST/query.sql"
"$BINDIR/psql" -v ON_ERROR_STOP=1 -p "$PORT" -d postgres \
	$MICROBENCH_PARAMS \
	-f "$MODULE_DIR/$TEST/query.sql"
