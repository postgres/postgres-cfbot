#!/usr/bin/env bash
#
# run_undo_bench.sh - UNDO Benchmark Suite Orchestrator
#
# Compares three scenarios:
#   baseline  - pristine master branch (no UNDO code)
#   undo_off  - undo branch with enable_undo=off (code-presence overhead)
#   undo_on   - undo branch with enable_undo=on  (full UNDO overhead/benefits)
#
# Usage:
#   ./run_undo_bench.sh
#
# Configuration (environment variables):
#   BENCH_BASE        - Working directory (default: /scratch/undo-bench)
#   REPO_DIR          - Source repo (default: auto-detect)
#   SHARED_BUFFERS    - PG shared_buffers (default: 1GB)
#   SCALES            - Row counts for SQL benchmarks (default: 10000 100000 1000000)
#   PGBENCH_SCALES    - pgbench scale factors (default: 10 50 100)
#   PGBENCH_CLIENTS   - Client counts (default: 1 4 8)
#   PGBENCH_DURATION  - Seconds per pgbench run (default: 60)
#   ITERATIONS        - Measurement iterations (default: 3, warmup=1 always)
#   BENCHMARKS        - Which to run (default: b1 b2 b3 b4 b5 b6 b7 b8 pgbench mixed)
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/lib/common.sh"
source "$SCRIPT_DIR/lib/report.sh"

###############################################################################
# Cleanup trap
###############################################################################

cleanup() {
    echo ""
    log "Cleaning up..."
    stop_metrics 2>/dev/null || true
    stop_all_clusters
    # Clean up git worktrees (skip symlinks)
    for _branch in master undo; do
        if [ -d "$SRC_DIR/$_branch" ] && [ ! -L "$SRC_DIR/$_branch" ]; then
            (cd "$REPO_DIR" && git worktree remove "$SRC_DIR/$_branch" --force 2>/dev/null) || true
        fi
    done
    log "Done. Results in $RESULTS_DIR/"
}

trap cleanup EXIT

###############################################################################
# Banner
###############################################################################

echo ""
echo "============================================================"
echo " UNDO Benchmark Suite"
echo " Scenarios: baseline | undo_off | undo_on"
echo " Benchmarks: $BENCHMARKS"
echo " SQL scales: $SCALES"
echo " pgbench scales: $PGBENCH_SCALES"
echo " Iterations: $ITERATIONS (+ 1 warmup)"
echo " BENCH_BASE: $BENCH_BASE"
echo "============================================================"
echo ""

###############################################################################
# Phase 0: Setup directories
###############################################################################

log "Phase 0: Setting up directories"
mkdir -p "$SRC_DIR" "$BUILD_DIR" "$INSTALL_DIR" "$DATA_DIR" "$RESULTS_DIR" "$LOGS_DIR"

record_sysinfo "$RESULTS_DIR/sysinfo.txt"
csv_init "$CSV_FILE"

###############################################################################
# Phase 1: Build both branches
###############################################################################

log "Phase 1: Building branches"
build_branch master
build_branch undo

###############################################################################
# Phase 2: Initialize clusters
###############################################################################

log "Phase 2: Initializing clusters"
for scenario in $SCENARIOS; do
    init_cluster "$scenario"
done

###############################################################################
# Phase 3: Run benchmarks
###############################################################################

log "Phase 3: Running benchmarks"

# run_sql_benchmark BENCH_NAME SQL_FILE
# Runs a SQL benchmark across all scenarios and scales
run_sql_benchmark() {
    local bench="$1"
    local sql_file="$2"
    local scales
    scales="$(get_bench_scales "$bench")"

    for scenario in $SCENARIOS; do
        start_cluster "$scenario"
        create_bench_db "$scenario"

        local create_opts
        create_opts="$(get_create_opts "$scenario")"

        for scale in $scales; do
            log "  $bench / $scenario / scale=$scale"

            # Warmup iteration (discarded)
            log "    Warmup..."
            run_psql "$scenario" "$sql_file" \
                "scenario=$scenario" "row_count=$scale" "create_opts=$create_opts" \
                >/dev/null 2>&1 || true
            run_checkpoint "$scenario"

            # Measurement iterations
            for iter in $(seq 1 "$ITERATIONS"); do
                log "    Iteration $iter/$ITERATIONS"
                local output
                output="$(run_psql "$scenario" "$sql_file" \
                    "scenario=$scenario" "row_count=$scale" "create_opts=$create_opts")"

                # Extract and record results
                extract_results "$output" | while IFS=$'\t' read -r sub_test metric value; do
                    # Determine unit from metric name
                    local unit
                    case "$metric" in
                        time_ms) unit="ms" ;;
                        bytes)   unit="bytes" ;;
                        count)   unit="count" ;;
                        *)       unit="$metric" ;;
                    esac
                    csv_write "$CSV_FILE" "$scenario" "$bench" "$sub_test" \
                        "$scale" "$iter" "$metric" "$value" "$unit"
                done

                run_checkpoint "$scenario"
            done
        done

        stop_cluster "$scenario"
    done
}

# run_pgbench_benchmark BENCH_NAME [CUSTOM_SCRIPT]
# Runs pgbench across all scenarios, scales, and client counts.
# Collects system metrics (CPU, RAM, I/O) and VACUUM stats per run.
run_pgbench_benchmark() {
    local bench="$1"
    local custom_script="${2:-}"

    for scenario in $SCENARIOS; do
        start_cluster "$scenario"

        local bindir port libdir create_opts
        bindir="$(get_bindir "$scenario")"
        port="$(get_port "$scenario")"
        libdir="$(get_libdir "$scenario")"
        create_opts="$(get_create_opts "$scenario")"

        export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        export DYLD_LIBRARY_PATH="${libdir}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

        # Enable autovacuum for pgbench runs so we can track VACUUM frequency
        "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres -X --no-psqlrc \
            -c "ALTER SYSTEM SET autovacuum = on;" \
            -c "ALTER SYSTEM SET autovacuum_naptime = '15s';" \
            -c "SELECT pg_reload_conf();" \
            >/dev/null 2>&1

        for scale in $PGBENCH_SCALES; do
            log "  $bench / $scenario / scale=$scale: initializing pgbench tables"

            # Initialize pgbench tables
            "$bindir/pgbench" -i -s "$scale" -h 127.0.0.1 -p "$port" postgres \
                >"$LOGS_DIR/pgbench_init_${scenario}_${scale}.log" 2>&1

            # Enable UNDO on pgbench tables for undo_on scenario.
            if [ "$scenario" = "undo_on" ]; then
                "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres -X --no-psqlrc \
                    -c "ALTER TABLE pgbench_accounts SET (enable_undo = on);" \
                    -c "ALTER TABLE pgbench_tellers SET (enable_undo = on);" \
                    -c "ALTER TABLE pgbench_branches SET (enable_undo = on);" \
                    -c "ALTER TABLE pgbench_history SET (enable_undo = on);" \
                    >/dev/null 2>&1
            fi

            for clients in $PGBENCH_CLIENTS; do
                log "  $bench / $scenario / scale=$scale / clients=$clients"

                # Build pgbench command
                local pgbench_args=(-h 127.0.0.1 -p "$port"
                    -c "$clients" -j "$clients"
                    -T "$PGBENCH_DURATION" --no-vacuum postgres)

                if [ -n "$custom_script" ]; then
                    pgbench_args+=(-f "$custom_script")
                fi

                # Prewarm buffers for in-cache benchmarks (scale <= 100)
                if [ "$scale" -le 100 ] 2>/dev/null; then
                    warm_buffers "$scenario" postgres \
                        pgbench_accounts pgbench_branches pgbench_tellers
                fi

                # Warmup
                log "    Warmup..."
                "$bindir/pgbench" "${pgbench_args[@]}" \
                    >"$LOGS_DIR/pgbench_warmup_${scenario}_${scale}_${clients}.log" 2>&1 || true

                # Measurement iterations
                for iter in $(seq 1 "$ITERATIONS"); do
                    log "    Iteration $iter/$ITERATIONS"

                    local metrics_label="${bench}_${scenario}_s${scale}_c${clients}_i${iter}"

                    # Snapshot VACUUM stats before run
                    local vac_before="$LOGS_DIR/vacstats_before_${metrics_label}.tsv"
                    get_vacuum_stats "$scenario" postgres > "$vac_before"

                    # Record RSS before
                    local rss_before
                    rss_before="$(get_pg_rss "$scenario")"

                    # Start system metrics collection
                    start_metrics "$metrics_label"

                    # Start wait event sampler
                    start_wait_sampler "$scenario" postgres \
                        "$LOGS_DIR/waits_${metrics_label}.txt" 2

                    # Run pgbench
                    local output
                    output=$("$bindir/pgbench" "${pgbench_args[@]}" 2>&1) || true

                    # Stop wait sampler and system metrics
                    stop_wait_sampler
                    stop_metrics

                    # Record RSS after
                    local rss_after
                    rss_after="$(get_pg_rss "$scenario")"

                    # Parse TPS (PG19: "without initial connection time")
                    local tps lat
                    tps=$(echo "$output" | grep -iE "without initial connection|excluding connections" \
                        | sed 's/.*= *//' | sed 's/ .*//' || echo "")
                    lat=$(echo "$output" | grep -i "latency average" \
                        | sed 's/.*= *//' | sed 's/ .*//' || echo "")

                    [ -z "$tps" ] && tps="0"
                    [ -z "$lat" ] && lat="0"

                    # Record TPS and latency
                    csv_write "$CSV_FILE" "$scenario" "$bench" \
                        "tps_c${clients}" "$scale" "$iter" "tps" "$tps" "tps"
                    csv_write "$CSV_FILE" "$scenario" "$bench" \
                        "lat_c${clients}" "$scale" "$iter" "latency_ms" "$lat" "ms"

                    # Record system metrics (CPU, I/O)
                    record_metrics "$CSV_FILE" "$scenario" "$bench" "c${clients}" \
                        "$scale" "$iter" \
                        "$LOGS_DIR/metrics_${metrics_label}_vmstat.log" \
                        "$LOGS_DIR/metrics_${metrics_label}_iostat.log"

                    # Record RAM (RSS in kB)
                    csv_write "$CSV_FILE" "$scenario" "$bench" \
                        "c${clients}_rss_before_kb" "$scale" "$iter" "kB" "$rss_before" "kB"
                    csv_write "$CSV_FILE" "$scenario" "$bench" \
                        "c${clients}_rss_after_kb" "$scale" "$iter" "kB" "$rss_after" "kB"

                    # Snapshot VACUUM stats after run and record deltas
                    local vac_after="$LOGS_DIR/vacstats_after_${metrics_label}.tsv"
                    get_vacuum_stats "$scenario" postgres > "$vac_after"
                    record_vacuum_delta "$CSV_FILE" "$scenario" "$bench" \
                        "$scale" "$iter" "$vac_before" "$vac_after"
                done

                run_checkpoint "$scenario"
            done
        done

        # Disable autovacuum again before stopping
        "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres -X --no-psqlrc \
            -c "ALTER SYSTEM RESET autovacuum;" \
            -c "ALTER SYSTEM RESET autovacuum_naptime;" \
            >/dev/null 2>&1 || true

        stop_cluster "$scenario"
    done
}

# Measure UNDO log directory size for undo_on scenario
measure_undo_log_size() {
    local scenario="$1"
    local bench="$2"
    local scale="$3"
    local iter="$4"

    if [ "$scenario" = "undo_on" ]; then
        local pgdata undo_dir size
        pgdata="$(get_pgdata "$scenario")"
        undo_dir="$pgdata/base/undo"
        if [ -d "$undo_dir" ]; then
            size=$(get_dir_bytes "$undo_dir")
            [ -n "$size" ] && csv_write "$CSV_FILE" "$scenario" "$bench" \
                "undo_log_size" "$scale" "$iter" "bytes" "$size" "bytes"
        fi
    fi
}

# ── Run each benchmark ────────────────────────────────────────────────────────

for bench in $BENCHMARKS; do
    log "=== Benchmark: $bench ==="

    # Re-initialize clusters between benchmarks to avoid stale UNDO log files
    for scenario in $SCENARIOS; do
        init_cluster "$scenario"
    done

    case "$bench" in
        b1)
            run_sql_benchmark b1 "$SCRIPT_DIR/sql/b1_insert_throughput.sql"
            ;;
        b2)
            run_sql_benchmark b2 "$SCRIPT_DIR/sql/b2_update_performance.sql"
            ;;
        b3)
            run_sql_benchmark b3 "$SCRIPT_DIR/sql/b3_delete_performance.sql"
            ;;
        b4)
            run_sql_benchmark b4 "$SCRIPT_DIR/sql/b4_read_under_writes.sql"
            ;;
        b5)
            run_sql_benchmark b5 "$SCRIPT_DIR/sql/b5_rollback_cost.sql"
            ;;
        b6)
            run_sql_benchmark b6 "$SCRIPT_DIR/sql/b6_vacuum_overhead.sql"
            ;;
        b7)
            # B7 needs UNDO log size measurement after SQL run
            _run_b7() {
                local b7_scales
                b7_scales="$(get_bench_scales b7)"
                for scenario in $SCENARIOS; do
                    start_cluster "$scenario"
                    create_bench_db "$scenario"
                    local create_opts
                    create_opts="$(get_create_opts "$scenario")"
                    for scale in $b7_scales; do
                        log "  b7 / $scenario / scale=$scale"
                        # Warmup
                        run_psql "$scenario" "$SCRIPT_DIR/sql/b7_storage_footprint.sql" \
                            "scenario=$scenario" "row_count=$scale" "create_opts=$create_opts" \
                            >/dev/null 2>&1 || true
                        run_checkpoint "$scenario"
                        for iter in $(seq 1 "$ITERATIONS"); do
                            log "    Iteration $iter/$ITERATIONS"
                            local output
                            output="$(run_psql "$scenario" "$SCRIPT_DIR/sql/b7_storage_footprint.sql" \
                                "scenario=$scenario" "row_count=$scale" "create_opts=$create_opts")"
                            local sub_test metric value unit
                            extract_results "$output" | while IFS=$'\t' read -r sub_test metric value; do
                                case "$metric" in
                                    time_ms) unit="ms" ;;
                                    bytes)   unit="bytes" ;;
                                    count)   unit="count" ;;
                                    *)       unit="$metric" ;;
                                esac
                                csv_write "$CSV_FILE" "$scenario" "b7" "$sub_test" \
                                    "$scale" "$iter" "$metric" "$value" "$unit"
                            done
                            # Measure UNDO log directory size
                            measure_undo_log_size "$scenario" "b7" "$scale" "$iter"
                            run_checkpoint "$scenario"
                        done
                    done
                    stop_cluster "$scenario"
                done
            }
            _run_b7
            ;;
        b8)
            run_sql_benchmark b8 "$SCRIPT_DIR/sql/b8_large_transaction.sql"
            ;;
        pgbench)
            run_pgbench_benchmark pgbench
            ;;
        mixed)
            run_pgbench_benchmark mixed "$SCRIPT_DIR/pgbench/mixed_oltp.sql"
            ;;
        zipfian)
            # Zipfian hot/cold workload: skewed access pattern.
            # Uses larger scale factor to create realistic cache-pressure.
            PGBENCH_SCALES="$PGBENCH_SCALE_LARGE" \
                run_pgbench_benchmark zipfian "$SCRIPT_DIR/pgbench/zipfian_hot_cold.sql"
            ;;
        concurrent)
            # Multi-role concurrent workload (W9-style):
            # 4 concurrent pgbench instances with different behaviors,
            # all hitting the same database simultaneously.
            _run_concurrent() {
                for scenario in $SCENARIOS; do
                    start_cluster "$scenario"

                    local bindir port libdir create_opts
                    bindir="$(get_bindir "$scenario")"
                    port="$(get_port "$scenario")"
                    libdir="$(get_libdir "$scenario")"
                    create_opts="$(get_create_opts "$scenario")"

                    export LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                    export DYLD_LIBRARY_PATH="${libdir}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

                    local scale="$PGBENCH_SCALE_LARGE"
                    log "  concurrent / $scenario / scale=$scale: initializing"

                    # Initialize pgbench tables at large scale
                    "$bindir/pgbench" -i -s "$scale" -h 127.0.0.1 -p "$port" postgres \
                        >"$LOGS_DIR/pgbench_init_concurrent_${scenario}.log" 2>&1

                    # Enable UNDO on pgbench tables for undo_on
                    if [ "$scenario" = "undo_on" ]; then
                        "$bindir/psql" -h 127.0.0.1 -p "$port" -d postgres -X --no-psqlrc \
                            -c "ALTER TABLE pgbench_accounts SET (enable_undo = on);" \
                            -c "ALTER TABLE pgbench_tellers SET (enable_undo = on);" \
                            -c "ALTER TABLE pgbench_branches SET (enable_undo = on);" \
                            -c "ALTER TABLE pgbench_history SET (enable_undo = on);" \
                            >/dev/null 2>&1
                    fi

                    # Divide clients across 4 roles: 30% hot read, 30% cold read,
                    # 20% updater, 20% scanner
                    local total_clients=16
                    local hot_c=5 cold_c=5 upd_c=3 scan_c=3

                    for iter in $(seq 1 "$ITERATIONS"); do
                        log "  concurrent / $scenario / iteration=$iter"
                        local label="concurrent_${scenario}_i${iter}"

                        # Start wait event sampler
                        start_wait_sampler "$scenario" postgres \
                            "$LOGS_DIR/waits_${label}.txt" 2

                        # Start system metrics
                        start_metrics "$label"

                        # Launch 4 roles in parallel
                        "$bindir/pgbench" -h 127.0.0.1 -p "$port" \
                            -c "$hot_c" -j "$hot_c" \
                            -T "$PGBENCH_DURATION" --no-vacuum \
                            -f "$SCRIPT_DIR/pgbench/role_hot_reader.sql" \
                            postgres >"$LOGS_DIR/pgb_hot_${label}.log" 2>&1 &
                        local pid_hot=$!

                        "$bindir/pgbench" -h 127.0.0.1 -p "$port" \
                            -c "$cold_c" -j "$cold_c" \
                            -T "$PGBENCH_DURATION" --no-vacuum \
                            -f "$SCRIPT_DIR/pgbench/role_cold_reader.sql" \
                            postgres >"$LOGS_DIR/pgb_cold_${label}.log" 2>&1 &
                        local pid_cold=$!

                        "$bindir/pgbench" -h 127.0.0.1 -p "$port" \
                            -c "$upd_c" -j "$upd_c" \
                            -T "$PGBENCH_DURATION" --no-vacuum \
                            -f "$SCRIPT_DIR/pgbench/role_updater.sql" \
                            postgres >"$LOGS_DIR/pgb_upd_${label}.log" 2>&1 &
                        local pid_upd=$!

                        "$bindir/pgbench" -h 127.0.0.1 -p "$port" \
                            -c "$scan_c" -j "$scan_c" \
                            -T "$PGBENCH_DURATION" --no-vacuum \
                            -f "$SCRIPT_DIR/pgbench/role_scanner.sql" \
                            postgres >"$LOGS_DIR/pgb_scan_${label}.log" 2>&1 &
                        local pid_scan=$!

                        # Wait for all roles to finish
                        wait $pid_hot $pid_cold $pid_upd $pid_scan 2>/dev/null || true

                        # Stop metrics and sampler
                        stop_metrics
                        stop_wait_sampler

                        # Extract TPS from each role
                        for role in hot cold upd scan; do
                            local logf="$LOGS_DIR/pgb_${role}_${label}.log"
                            local tps_val
                            tps_val=$(grep -iE "without initial connection|excluding connections" "$logf" \
                                | sed 's/.*= *//' | sed 's/ .*//' 2>/dev/null || echo "0")
                            [ -z "$tps_val" ] && tps_val="0"
                            csv_write "$CSV_FILE" "$scenario" "concurrent" \
                                "tps_${role}" "$scale" "$iter" "tps" "$tps_val" "tps"
                        done

                        # Aggregate total TPS across all roles
                        local total_tps=0
                        for role in hot cold upd scan; do
                            local logf="$LOGS_DIR/pgb_${role}_${label}.log"
                            local t
                            t=$(grep -iE "without initial connection|excluding connections" "$logf" \
                                | sed 's/.*= *//' | sed 's/ .*//' 2>/dev/null || echo "0")
                            [ -z "$t" ] && t="0"
                            total_tps=$(echo "$total_tps $t" | awk '{printf "%.1f", $1+$2}')
                        done
                        csv_write "$CSV_FILE" "$scenario" "concurrent" \
                            "tps_total" "$scale" "$iter" "tps" "$total_tps" "tps"

                        # Record wait event summary
                        local wait_summary
                        wait_summary="$(summarize_wait_events "$LOGS_DIR/waits_${label}.txt")"
                        if [ -n "$wait_summary" ]; then
                            echo "$wait_summary" | while IFS='|' read -r wtype wevent wcount wpct; do
                                csv_write "$CSV_FILE" "$scenario" "concurrent" \
                                    "wait_${wtype}_${wevent}" "$scale" "$iter" "samples" "$wcount" "samples"
                            done
                        fi

                        run_checkpoint "$scenario"
                    done

                    stop_cluster "$scenario"
                done
            }
            _run_concurrent
            ;;
        *)
            log "WARNING: Unknown benchmark '$bench', skipping"
            ;;
    esac
done

###############################################################################
# Phase 4: Generate report
###############################################################################

log "Phase 4: Generating report"
generate_report "$CSV_FILE" "$RESULTS_DIR/sysinfo.txt" "$RESULTS_DIR/summary.txt"

echo ""
log "Benchmark complete."
log "CSV:     $CSV_FILE"
log "Report:  $RESULTS_DIR/summary.txt"
log "Logs:    $LOGS_DIR/"
