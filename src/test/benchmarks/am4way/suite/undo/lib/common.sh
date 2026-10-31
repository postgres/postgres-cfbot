#!/usr/bin/env bash
#
# common.sh - Shared helpers for UNDO benchmark suite
#
# Provides: build, cluster init/start/stop, psql runner, timing,
# result extraction, and configuration defaults.
#

# Resolve paths
UNDO_BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$UNDO_BENCH_DIR/../../../.." && pwd)"

# Configuration defaults (override via environment)
BENCH_BASE="${BENCH_BASE:-/scratch/undo-bench}"
REPO_DIR="${REPO_DIR:-$REPO_ROOT}"
SHARED_BUFFERS="${SHARED_BUFFERS:-1GB}"
SCALES="${SCALES:-10000 100000 1000000}"
PGBENCH_SCALES="${PGBENCH_SCALES:-10 50 100}"
PGBENCH_CLIENTS="${PGBENCH_CLIENTS:-1 4 8}"
PGBENCH_DURATION="${PGBENCH_DURATION:-60}"
ITERATIONS="${ITERATIONS:-3}"
BENCHMARKS="${BENCHMARKS:-b1 b2 b3 b4 b5 b6 b7 b8 pgbench mixed zipfian concurrent}"
# Large-scale factor for cache-pressure workloads (working set >> shared_buffers)
PGBENCH_SCALE_LARGE="${PGBENCH_SCALE_LARGE:-500}"

# Ports for three scenarios
PORT_BASELINE=54320
PORT_UNDO_OFF=54321
PORT_UNDO_ON=54322

# Directory layout under BENCH_BASE
SRC_DIR="$BENCH_BASE/src"
BUILD_DIR="$BENCH_BASE/build"
INSTALL_DIR="$BENCH_BASE/install"
DATA_DIR="$BENCH_BASE/data"
RESULTS_DIR="$BENCH_BASE/results"
LOGS_DIR="$BENCH_BASE/logs"
CSV_FILE="$RESULTS_DIR/undo_bench_results.csv"

# All scenarios
SCENARIOS="baseline undo_off undo_on"

###############################################################################
# Logging
###############################################################################

log() {
    echo "[$(date '+%H:%M:%S')] $*"
}

die() {
    echo "FATAL: $*" >&2
    exit 1
}

###############################################################################
# Portable helpers
###############################################################################

# get_nproc; portable CPU count (Linux, FreeBSD, Illumos, macOS)
get_nproc() {
    nproc 2>/dev/null \
        || getconf _NPROCESSORS_ONLN 2>/dev/null \
        || psrinfo 2>/dev/null | wc -l | tr -d ' ' \
        || sysctl -n hw.ncpu 2>/dev/null \
        || echo 1
}

# get_dir_bytes DIR; portable directory size in bytes
get_dir_bytes() {
    local dir="$1"
    if du -sb "$dir" >/dev/null 2>&1; then
        du -sb "$dir" | awk '{print $1}'
    elif du -sk "$dir" >/dev/null 2>&1; then
        du -sk "$dir" | awk '{print $1 * 1024}'
    else
        echo 0
    fi
}

###############################################################################
# System info
###############################################################################

record_sysinfo() {
    local outfile="$1"
    {
        echo "hostname: $(hostname)"
        echo "date: $(date -Iseconds 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S')"
        echo "kernel: $(uname -sr)"
        echo "arch: $(uname -m)"

        # CPU identification; Linux, Illumos/Solaris, FreeBSD/macOS
        if [ -f /proc/cpuinfo ]; then
            echo "cpu: $(grep 'model name' /proc/cpuinfo | head -1 | sed 's/.*: //')"
        elif command -v psrinfo >/dev/null 2>&1; then
            echo "cpu: $(psrinfo -pv 2>/dev/null | grep -i 'MHz\|GHz\|SPARC\|processor' | head -1 | sed 's/^ *//')"
        elif command -v sysctl >/dev/null 2>&1; then
            echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -p)"
        else
            echo "cpu: $(uname -p 2>/dev/null || echo 'unknown')"
        fi

        echo "cores: $(get_nproc)"

        # Memory; Linux, Illumos/Solaris, FreeBSD/macOS
        if command -v free >/dev/null 2>&1; then
            echo "ram: $(free -h 2>/dev/null | awk '/^Mem:/{print $2}')"
        elif command -v prtconf >/dev/null 2>&1; then
            echo "ram: $(prtconf 2>/dev/null | grep -i 'Memory size' | sed 's/.*: //')"
        elif command -v sysctl >/dev/null 2>&1; then
            echo "ram: $(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%.0fG", $1/1073741824}')"
        else
            echo "ram: unknown"
        fi

        echo "postgres_commit_master: $(cd "$REPO_DIR" && git rev-parse --short master 2>/dev/null || echo 'unknown')"
        echo "postgres_commit_undo: $(cd "$REPO_DIR" && git rev-parse --short undo 2>/dev/null || echo 'unknown')"
    } > "$outfile"
    log "System info written to $outfile"
}

###############################################################################
# Build
###############################################################################

build_branch() {
    local branch="$1"
    local src="$SRC_DIR/$branch"
    local build="$BUILD_DIR/$branch"
    local install="$INSTALL_DIR/$branch"

    log "Building branch: $branch"

    # Create worktree if needed.  If the branch is already checked out in
    # the main working tree (common when running from the undo branch),
    # fall back to a detached worktree from the branch's HEAD, or just
    # symlink the repo directory.
    if [ ! -d "$src" ]; then
        log "  Creating git worktree for $branch"
        if (cd "$REPO_DIR" && git worktree add "$src" "$branch") \
                >>"$LOGS_DIR/build_${branch}.log" 2>&1; then
            : # success
        else
            log "  Worktree add failed (branch may be checked out); trying detached HEAD"
            local branch_sha
            branch_sha="$(cd "$REPO_DIR" && git rev-parse "$branch")"
            if (cd "$REPO_DIR" && git worktree add --detach "$src" "$branch_sha") \
                    >>"$LOGS_DIR/build_${branch}.log" 2>&1; then
                : # success via detached HEAD
            else
                log "  Detached worktree also failed; symlinking repo directory"
                ln -sfn "$REPO_DIR" "$src"
            fi
        fi
    fi

    # Meson setup
    if [ ! -f "$build/build.ninja" ]; then
        log "  Running meson setup"
        # Detect platform-specific meson options
        local extra_meson_opts=""
        case "$(uname -s)" in
            SunOS|illumos)
                # Illumos: disable LDAP (ldap_start_tls_s linking issue)
                extra_meson_opts="-Dldap=disabled"
                ;;
        esac
        meson setup "$build" "$src" \
            --prefix="$install" \
            -Dbuildtype=release \
            -Dcassert=false \
            -Dtap_tests=disabled \
            $extra_meson_opts \
            >>"$LOGS_DIR/build_${branch}.log" 2>&1
    fi

    # Build and install
    log "  Compiling (ninja -j$(get_nproc))"
    ninja -C "$build" -j"$(get_nproc)" >>"$LOGS_DIR/build_${branch}.log" 2>&1
    log "  Installing to $install"
    DESTDIR= ninja -C "$build" install >>"$LOGS_DIR/build_${branch}.log" 2>&1

    log "  Build complete: $install"
}

###############################################################################
# Scenario helpers
###############################################################################

get_bindir() {
    local scenario="$1"
    case "$scenario" in
        baseline) echo "$INSTALL_DIR/master/bin" ;;
        undo_off|undo_on) echo "$INSTALL_DIR/undo/bin" ;;
        *) die "Unknown scenario: $scenario" ;;
    esac
}

get_libdir() {
    local scenario="$1"
    local bindir parent
    bindir="$(get_bindir "$scenario")"
    parent="$(dirname "$bindir")"
    for libdir in "$parent/lib64" "$parent/lib"; do
        if [ -d "$libdir" ]; then
            echo "$libdir"
            return
        fi
    done
    echo "$parent/lib"
}

get_port() {
    case "$1" in
        baseline) echo "$PORT_BASELINE" ;;
        undo_off) echo "$PORT_UNDO_OFF" ;;
        undo_on)  echo "$PORT_UNDO_ON" ;;
        *) die "Unknown scenario: $1" ;;
    esac
}

get_pgdata() {
    echo "$DATA_DIR/$1"
}

get_create_opts() {
    case "$1" in
        undo_on) echo "WITH (enable_undo = on)" ;;
        *)       echo "" ;;
    esac
}

# Returns the scales to iterate for a given benchmark
get_bench_scales() {
    local bench="$1"
    case "$bench" in
        b5)           echo "100000" ;;          # B5 manages internal sizes
        pgbench|mixed) echo "$PGBENCH_SCALES" ;;
        *)            echo "$SCALES" ;;
    esac
}

###############################################################################
# Cluster management
###############################################################################

init_cluster() {
    local scenario="$1"
    local bindir pgdata port
    bindir="$(get_bindir "$scenario")"
    pgdata="$(get_pgdata "$scenario")"
    port="$(get_port "$scenario")"

    log "Initializing cluster: $scenario (port $port)"

    rm -rf "$pgdata"
    mkdir -p "$pgdata"
    "$bindir/initdb" -D "$pgdata" --no-locale -E UTF8 -A trust \
        >"$LOGS_DIR/initdb_${scenario}.log" 2>&1

    # Common configuration
    {
        echo "port = $port"
        echo "listen_addresses = '127.0.0.1'"
        echo "unix_socket_directories = '$pgdata'"
        echo "shared_buffers = $SHARED_BUFFERS"
        echo "wal_level = minimal"
        echo "max_wal_senders = 0"
        echo "fsync = on"
        echo "synchronous_commit = on"
        echo "max_wal_size = 8GB"
        echo "checkpoint_timeout = 30min"
        echo "log_checkpoints = on"
        echo "autovacuum = off"
        echo "max_connections = 100"
        echo "logging_collector = off"
        echo "log_min_messages = warning"
    } >> "$pgdata/postgresql.conf"

    # Scenario-specific configuration
    case "$scenario" in
        undo_off)
            echo "enable_undo = off" >> "$pgdata/postgresql.conf"
            ;;
        undo_on)
            echo "enable_undo = on" >> "$pgdata/postgresql.conf"
            ;;
        # baseline: no enable_undo line (master branch doesn't have it)
    esac

    log "  Cluster initialized: $pgdata"
}

start_cluster() {
    local scenario="$1"
    local bindir pgdata port libdir
    bindir="$(get_bindir "$scenario")"
    pgdata="$(get_pgdata "$scenario")"
    port="$(get_port "$scenario")"
    libdir="$(get_libdir "$scenario")"

    log "Starting cluster: $scenario (port $port)"

    # Set library path without accumulating duplicates
    export LD_LIBRARY_PATH="${libdir}"
    export DYLD_LIBRARY_PATH="${libdir}"

    # Retry pg_ctl start to handle TCP TIME_WAIT on port reuse
    local start_attempts=6
    local start_ok=0
    while [ "$start_attempts" -gt 0 ]; do
        if "$bindir/pg_ctl" start -D "$pgdata" \
            -l "$LOGS_DIR/server_${scenario}.log" \
            -w -t 30 >/dev/null 2>&1; then
            start_ok=1
            break
        fi
        start_attempts=$((start_attempts - 1))
        if [ "$start_attempts" -gt 0 ]; then
            log "  Port $port may be in TIME_WAIT, retrying in 5s ($start_attempts attempts left)"
            sleep 5
        fi
    done
    [ "$start_ok" -eq 1 ] || die "Failed to start $scenario cluster (check $LOGS_DIR/server_${scenario}.log)"

    # Wait for ready
    local retries=30
    while [ "$retries" -gt 0 ]; do
        if "$bindir/pg_isready" -h 127.0.0.1 -p "$port" >/dev/null 2>&1; then
            log "  Server ready on port $port"
            return 0
        fi
        retries=$((retries - 1))
        sleep 1
    done
    die "$scenario server on port $port did not become ready"
}

stop_cluster() {
    local scenario="$1"
    local bindir pgdata
    bindir="$(get_bindir "$scenario")"
    pgdata="$(get_pgdata "$scenario")"

    log "Stopping cluster: $scenario"
    "$bindir/pg_ctl" stop -D "$pgdata" -m fast -w >/dev/null 2>&1 || true
    # Brief wait for TCP sockets to leave TIME_WAIT before restarting
    sleep 1
}

stop_all_clusters() {
    for s in $SCENARIOS; do
        local bindir pgdata
        bindir="$(get_bindir "$s" 2>/dev/null)" || continue
        pgdata="$(get_pgdata "$s")"
        if [ -d "$pgdata" ]; then
            "$bindir/pg_ctl" stop -D "$pgdata" -m immediate >/dev/null 2>&1 || true
        fi
    done
}

create_bench_db() {
    local scenario="$1"
    local bindir port
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"

    "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres \
        -c "DROP DATABASE IF EXISTS undo_bench;" \
        -c "CREATE DATABASE undo_bench;" \
        >/dev/null 2>&1
}

###############################################################################
# SQL execution
###############################################################################

# run_psql SCENARIO SQL_FILE [VAR=VALUE ...]
# Runs a SQL file via psql, returns output on stdout
run_psql() {
    local scenario="$1"
    local sql_file="$2"
    shift 2

    local bindir port libdir
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"
    libdir="$(get_libdir "$scenario")"

    export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export DYLD_LIBRARY_PATH="${libdir}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

    local var_args=()
    for var in "$@"; do
        var_args+=(-v "$var")
    done

    "$bindir/psql" -h 127.0.0.1 -p "$port" -d undo_bench \
        -X --no-psqlrc \
        "${var_args[@]}" \
        -f "$sql_file" 2>&1
}

run_checkpoint() {
    local scenario="$1"
    local bindir port
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"

    "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres \
        -X --no-psqlrc -c "CHECKPOINT;" >/dev/null 2>&1
}

###############################################################################
# Result extraction
###############################################################################

# extract_results OUTPUT
# Parses UNDO_BENCH_RESULT lines, outputs: sub_test\tmetric\tvalue
extract_results() {
    echo "$1" | grep '^UNDO_BENCH_RESULT|' | while IFS='|' read -r _marker sub_test metric value; do
        # Trim whitespace
        sub_test="$(echo "$sub_test" | tr -d '[:space:]')"
        metric="$(echo "$metric" | tr -d '[:space:]')"
        value="$(echo "$value" | tr -d '[:space:]')"
        printf '%s\t%s\t%s\n' "$sub_test" "$metric" "$value"
    done
}

# median VALUE1 VALUE2 ...
# Outputs the median of numeric arguments
median() {
    local n=$#
    if [ "$n" -eq 0 ]; then
        echo "0"
        return
    fi
    printf '%s\n' "$@" | sort -g | awk '{a[NR]=$1} END {
        if (NR % 2 == 1)
            print a[int(NR/2)+1]
        else
            printf "%.2f", (a[NR/2] + a[NR/2+1]) / 2
    }'
}

###############################################################################
# System metrics collection (CPU, RAM, I/O)
###############################################################################

# _METRICS_PID tracks background vmstat/iostat processes
_METRICS_VMSTAT_PID=""
_METRICS_IOSTAT_PID=""

# start_metrics LABEL
# Starts background vmstat + iostat sampling at 1-second intervals.
# Output goes to $LOGS_DIR/metrics_${LABEL}_{vmstat,iostat}.log
start_metrics() {
    local label="$1"
    local vmstat_log="$LOGS_DIR/metrics_${label}_vmstat.log"
    local iostat_log="$LOGS_DIR/metrics_${label}_iostat.log"

    # vmstat: CPU (us/sy/id/wa), memory, swap, I/O; portable across Linux/FreeBSD
    vmstat 1 > "$vmstat_log" 2>&1 &
    _METRICS_VMSTAT_PID=$!

    # iostat: disk I/O; use different flags per platform
    if [ "$(uname -s)" = "FreeBSD" ]; then
        iostat -x -w 1 > "$iostat_log" 2>&1 &
    elif [ "$(uname -s)" = "Linux" ]; then
        iostat -x 1 > "$iostat_log" 2>&1 &
    else
        # Fallback: just run iostat with default flags
        iostat 1 > "$iostat_log" 2>&1 &
    fi
    _METRICS_IOSTAT_PID=$!
}

# stop_metrics
# Stops background metric collectors started by start_metrics.
stop_metrics() {
    [ -n "$_METRICS_VMSTAT_PID" ] && kill "$_METRICS_VMSTAT_PID" 2>/dev/null && wait "$_METRICS_VMSTAT_PID" 2>/dev/null || true
    [ -n "$_METRICS_IOSTAT_PID" ] && kill "$_METRICS_IOSTAT_PID" 2>/dev/null && wait "$_METRICS_IOSTAT_PID" 2>/dev/null || true
    _METRICS_VMSTAT_PID=""
    _METRICS_IOSTAT_PID=""
}

# summarize_vmstat LOG_FILE
# Parses a vmstat log and outputs: avg_user_cpu avg_sys_cpu avg_idle avg_wa avg_free_mem
# Skips the first data line (boot-time avg). Handles both Linux (us/sy/id/wa)
# and FreeBSD (us/sy/id, no wa) vmstat formats.
summarize_vmstat() {
    local log_file="$1"
    awk '
    # Detect column layout from the header line containing "us" and "sy"
    /us.*sy.*id/ {
        for (i=1; i<=NF; i++) {
            if ($i == "us") col_us = i
            if ($i == "sy" && i > NF-5) col_sy = i  # rightmost "sy" is CPU
            if ($i == "id") col_id = i
            if ($i == "wa") col_wa = i
            if ($i == "fre") col_free = i
            if ($i == "free") col_free = i
        }
        next
    }
    /^ *[0-9]/ && col_us > 0 {
        lines++
        if (lines <= 1) next   # skip first sample (boot-time average)
        n++
        us += $col_us; sy += $col_sy; id += $col_id
        if (col_wa > 0) wa += $col_wa
        if (col_free > 0) free += $col_free
    }
    END {
        if (n > 0)
            printf "%.1f %.1f %.1f %.1f %.0f\n", us/n, sy/n, id/n, wa/n, free/n
        else
            print "0 0 0 0 0"
    }' "$log_file"
}

# summarize_iostat LOG_FILE
# Outputs: avg_read_kBs avg_write_kBs avg_busy_pct
# Works for both Linux and FreeBSD extended iostat output.
# Only considers the primary block device (nda0, sda, nvme0n1, etc.).
summarize_iostat() {
    local log_file="$1"
    awk '
    BEGIN { n=0; rkB=0; wkB=0; busy=0 }
    # Match device lines: starts with a device name, has numeric fields
    # FreeBSD: nda0  r/s w/s kr/s kw/s ms/r ms/w ms/o ms/t qlen %b  (11 fields)
    # Linux:   sda   rrqm/s wrqm/s r/s w/s rkB/s wkB/s ... %util   (14+ fields)
    # Only pick real disk devices (skip pass0, loop, dm-)
    /^(nda|ada|da|sd|nvme|vd)/ && NF >= 5 {
        n++
        if (NF >= 14) {
            # Linux extended: rkB/s=$6, wkB/s=$7, %util=$NF
            rkB += $6; wkB += $7; busy += $NF
        } else {
            # FreeBSD extended: kr/s=$4, kw/s=$5, %b=$NF
            rkB += $4; wkB += $5; busy += $NF
        }
    }
    END {
        if (n > 0)
            printf "%.1f %.1f %.1f\n", rkB/n, wkB/n, busy/n
        else
            print "0 0 0"
    }' "$log_file"
}

# record_metrics CSV_FILE SCENARIO BENCH SUB_LABEL SCALE ITER VMSTAT_LOG IOSTAT_LOG
# Parses collected metrics and writes them to the CSV.
record_metrics() {
    local csv_file="$1" scenario="$2" bench="$3" sub_label="$4"
    local scale="$5" iter="$6" vmstat_log="$7" iostat_log="$8"

    if [ -f "$vmstat_log" ]; then
        local vm_stats
        vm_stats="$(summarize_vmstat "$vmstat_log")"
        local cpu_user cpu_sys cpu_idle cpu_wa mem_free
        read -r cpu_user cpu_sys cpu_idle cpu_wa mem_free <<< "$vm_stats"

        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_cpu_user" "$scale" "$iter" "pct" "$cpu_user" "pct"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_cpu_sys" "$scale" "$iter" "pct" "$cpu_sys" "pct"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_cpu_idle" "$scale" "$iter" "pct" "$cpu_idle" "pct"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_cpu_iowait" "$scale" "$iter" "pct" "$cpu_wa" "pct"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_mem_free_kb" "$scale" "$iter" "kB" "$mem_free" "kB"
    fi

    if [ -f "$iostat_log" ]; then
        local io_stats
        io_stats="$(summarize_iostat "$iostat_log")"
        local io_read io_write io_busy
        read -r io_read io_write io_busy <<< "$io_stats"

        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_io_read_kBs" "$scale" "$iter" "kB/s" "$io_read" "kB/s"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_io_write_kBs" "$scale" "$iter" "kB/s" "$io_write" "kB/s"
        csv_write "$csv_file" "$scenario" "$bench" "${sub_label}_io_busy" "$scale" "$iter" "pct" "$io_busy" "pct"
    fi
}

###############################################################################
# VACUUM stats collection
###############################################################################

# get_vacuum_stats SCENARIO DB_NAME
# Outputs JSON-ish line per table: relname vacuum_count autovacuum_count
get_vacuum_stats() {
    local scenario="$1"
    local dbname="${2:-postgres}"
    local bindir port
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"

    "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" -X --no-psqlrc -t -A -F$'\t' -c "
        SELECT pg_stat_force_next_flush();
        SELECT relname, vacuum_count, autovacuum_count,
               COALESCE(n_dead_tup, 0) AS dead_tuples,
               COALESCE(n_live_tup, 0) AS live_tuples
        FROM pg_stat_user_tables
        ORDER BY relname;
    " 2>/dev/null | grep -v '^$' | grep -v '^pg_stat_force_next_flush'
}

# record_vacuum_delta CSV_FILE SCENARIO BENCH SCALE ITER BEFORE_FILE AFTER_FILE
# Computes the delta in vacuum_count and autovacuum_count between snapshots.
record_vacuum_delta() {
    local csv_file="$1" scenario="$2" bench="$3"
    local scale="$4" iter="$5" before="$6" after="$7"

    # Sum up vacuum/autovacuum counts across all tables
    local vac_before autovac_before dead_before
    vac_before=$(awk -F'\t' '{s+=$2} END{print s+0}' "$before")
    autovac_before=$(awk -F'\t' '{s+=$3} END{print s+0}' "$before")
    dead_before=$(awk -F'\t' '{s+=$4} END{print s+0}' "$before")

    local vac_after autovac_after dead_after
    vac_after=$(awk -F'\t' '{s+=$2} END{print s+0}' "$after")
    autovac_after=$(awk -F'\t' '{s+=$3} END{print s+0}' "$after")
    dead_after=$(awk -F'\t' '{s+=$4} END{print s+0}' "$after")

    local vac_delta=$((vac_after - vac_before))
    local autovac_delta=$((autovac_after - autovac_before))

    csv_write "$csv_file" "$scenario" "$bench" "vacuum_count" "$scale" "$iter" "count" "$vac_delta" "count"
    csv_write "$csv_file" "$scenario" "$bench" "autovacuum_count" "$scale" "$iter" "count" "$autovac_delta" "count"
    csv_write "$csv_file" "$scenario" "$bench" "dead_tuples_end" "$scale" "$iter" "count" "$dead_after" "count"
}

###############################################################################
# pg_prewarm - deterministic cache state
###############################################################################

# warm_buffers SCENARIO DB_NAME TABLE_NAMES...
# Preloads heap relations and their indexes into shared_buffers.
# Ensures 100% buffer hit ratio for in-cache workloads.
warm_buffers() {
    local scenario="$1"
    local dbname="${2:-postgres}"
    shift 2
    local bindir port
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"

    log "  Warming buffers with pg_prewarm"

    # Ensure pg_prewarm extension exists
    "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" -X --no-psqlrc -q \
        -c "CREATE EXTENSION IF NOT EXISTS pg_prewarm;" 2>/dev/null || true

    # Prewarm each table and its indexes
    for tbl in "$@"; do
        "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" -X --no-psqlrc -q -c "
            SELECT pg_prewarm('${tbl}', 'buffer');
        " 2>/dev/null || true
        # Prewarm all indexes on this table
        "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" -X --no-psqlrc -t -A -q -c "
            SELECT indexrelid::regclass::text
            FROM pg_index WHERE indrelid = '${tbl}'::regclass;
        " 2>/dev/null | while read -r idx; do
            [ -n "$idx" ] && "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" \
                -X --no-psqlrc -q -c "SELECT pg_prewarm('${idx}', 'buffer');" 2>/dev/null || true
        done
    done
}

###############################################################################
# Wait event sampling
###############################################################################

# _WAIT_SAMPLER_PID tracks the background sampler process
_WAIT_SAMPLER_PID=""

# start_wait_sampler SCENARIO DB_NAME OUTPUT_FILE [INTERVAL_SECS]
# Samples pg_stat_activity wait events at the given interval.
# Runs in background; call stop_wait_sampler to terminate.
start_wait_sampler() {
    local scenario="$1"
    local dbname="$2"
    local outfile="$3"
    local interval="${4:-2}"
    local bindir port
    bindir="$(get_bindir "$scenario")"
    port="$(get_port "$scenario")"

    (
        echo "# Wait event samples: $scenario interval=${interval}s" > "$outfile"
        while true; do
            "$bindir/psql" -h 127.0.0.1 -p "$port" -d "$dbname" -t -A -q -c "
                SELECT now()::time, wait_event_type, wait_event, count(*)
                FROM pg_stat_activity
                WHERE state = 'active' AND pid != pg_backend_pid()
                GROUP BY 1,2,3
                ORDER BY 4 DESC;
            " >> "$outfile" 2>/dev/null
            sleep "$interval"
        done
    ) &
    _WAIT_SAMPLER_PID=$!
}

# stop_wait_sampler
# Terminates the background wait event sampler.
stop_wait_sampler() {
    if [ -n "$_WAIT_SAMPLER_PID" ]; then
        kill "$_WAIT_SAMPLER_PID" 2>/dev/null
        wait "$_WAIT_SAMPLER_PID" 2>/dev/null || true
        _WAIT_SAMPLER_PID=""
    fi
}

# summarize_wait_events OUTPUT_FILE
# Aggregates wait event samples and outputs the top events.
summarize_wait_events() {
    local outfile="$1"
    [ -f "$outfile" ] || return
    awk -F'|' '
    /^[0-9]/ && NF>=4 {
        key = $2 "|" $3
        count[key] += $4
        total += $4
    }
    END {
        for (k in count)
            printf "%s|%d|%.1f%%\n", k, count[k], count[k]*100/total
    }' "$outfile" | sort -t'|' -k2 -rn | head -10
}

###############################################################################
# Statistics helpers
###############################################################################

# cv VALUES...
# Computes coefficient of variation (CV%) from a list of numeric values.
# Returns 0.0 if fewer than 2 values.
cv() {
    if [ $# -lt 2 ]; then
        echo "0.0"
        return
    fi
    printf '%s\n' "$@" | awk '
    {a[NR]=$1; s+=$1}
    END {
        if (NR < 2) {print "0.0"; exit}
        avg = s / NR
        if (avg == 0) {print "0.0"; exit}
        for (i=1; i<=NR; i++) ss += (a[i] - avg)^2
        sd = sqrt(ss / (NR - 1))
        printf "%.1f", (sd / avg) * 100
    }'
}

# stdev VALUES...
# Computes sample standard deviation.
stdev() {
    if [ $# -lt 2 ]; then
        echo "0"
        return
    fi
    printf '%s\n' "$@" | awk '
    {a[NR]=$1; s+=$1}
    END {
        if (NR < 2) {print "0"; exit}
        avg = s / NR
        for (i=1; i<=NR; i++) ss += (a[i] - avg)^2
        printf "%.2f", sqrt(ss / (NR - 1))
    }'
}

# percentile P VALUES...
# Computes the P-th percentile (P in 0-100) using linear interpolation.
percentile() {
    local pct="$1"
    shift
    printf '%s\n' "$@" | sort -g | awk -v p="$pct" '
    {a[NR]=$1}
    END {
        if (NR == 0) {print "0"; exit}
        rank = (p / 100.0) * (NR - 1) + 1
        lo = int(rank)
        hi = lo + 1
        if (lo < 1) lo = 1
        if (hi > NR) hi = NR
        frac = rank - int(rank)
        printf "%.2f", a[lo] + frac * (a[hi] - a[lo])
    }'
}

###############################################################################
# Memory / RSS
###############################################################################

# get_pg_rss SCENARIO
# Returns RSS of the postgres backend processes in kB.
get_pg_rss() {
    local scenario="$1"
    local pgdata
    pgdata="$(get_pgdata "$scenario")"
    local pid_file="$pgdata/postmaster.pid"
    if [ -f "$pid_file" ]; then
        local main_pid
        main_pid=$(head -1 "$pid_file")
        # Sum RSS of all postgres processes in this cluster
        ps -o rss= -p "$main_pid" $(pgrep -P "$main_pid" 2>/dev/null) 2>/dev/null \
            | awk '{s+=$1} END{print s+0}'
    else
        echo "0"
    fi
}
