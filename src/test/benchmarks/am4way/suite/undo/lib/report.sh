#!/usr/bin/env bash
#
# report.sh - CSV collection and summary report generation
#

###############################################################################
# CSV helpers
###############################################################################

# csv_init FILE
csv_init() {
    local file="$1"
    echo "timestamp,scenario,benchmark,sub_test,scale,iteration,metric,value,unit" > "$file"
}

# csv_write FILE SCENARIO BENCHMARK SUB_TEST SCALE ITERATION METRIC VALUE UNIT
csv_write() {
    local file="$1" ts
    ts="$(date -Iseconds 2>/dev/null || date '+%Y-%m-%dT%H:%M:%S%z')"
    echo "${ts},$2,$3,$4,$5,$6,$7,$8,$9" >> "$file"
}

###############################################################################
# Report generation
###############################################################################

# generate_report CSV_FILE SYSINFO_FILE OUTPUT_FILE
generate_report() {
    local csv_file="$1"
    local sysinfo_file="$2"
    local output_file="$3"

    local cpu ram kernel hostname_val date_str
    hostname_val="$(grep '^hostname:' "$sysinfo_file" | sed 's/^hostname: *//')"
    cpu="$(grep '^cpu:' "$sysinfo_file" | sed 's/^cpu: *//')"
    ram="$(grep '^ram:' "$sysinfo_file" | sed 's/^ram: *//')"
    kernel="$(grep '^kernel:' "$sysinfo_file" | sed 's/^kernel: *//')"
    date_str="$(date '+%Y-%m-%d')"

    {
        echo "================================================================"
        echo " UNDO Benchmark Results - ${date_str} - ${hostname_val}"
        echo " CPU: ${cpu} | RAM: ${ram} | Kernel: ${kernel}"
        echo "================================================================"
        echo ""

        # Collect unique benchmarks in order
        local benchmarks
        benchmarks=$(awk -F, 'NR>1 {print $3}' "$csv_file" | awk '!seen[$0]++')

        for bench in $benchmarks; do
            local bench_label
            case "$bench" in
                b1) bench_label="B1: Insert Throughput" ;;
                b2) bench_label="B2: Update Performance" ;;
                b3) bench_label="B3: Delete Performance" ;;
                b4) bench_label="B4: Read Under Writes" ;;
                b5) bench_label="B5: Rollback Cost" ;;
                b6) bench_label="B6: VACUUM Overhead" ;;
                b7) bench_label="B7: Storage Footprint" ;;
                pgbench) bench_label="B8: pgbench TPS" ;;
                mixed) bench_label="B9: Mixed OLTP" ;;
                zipfian) bench_label="B10: Zipfian Hot/Cold" ;;
                concurrent) bench_label="B11: Multi-Role Concurrent" ;;
                *) bench_label="$bench" ;;
            esac

            # Get scales for this benchmark
            local scales
            scales=$(awk -F, -v b="$bench" '$3==b && NR>1 {print $5}' "$csv_file" | sort -nu)

            for scale in $scales; do
                echo " ${bench_label} (scale=${scale}, median of ${ITERATIONS:-3} iterations)"
                echo " ---------------------------------------------------------------"
                printf " %-24s | %10s | %10s | %10s | %8s | %8s | %5s\n" \
                    "Sub-test" "Baseline" "UNDO Off" "UNDO On" "Off/Base" "On/Base" "CV%"
                echo " ------------------------+------------+------------+------------+----------+----------+-------"

                # Get sub-tests for this benchmark+scale, preserving order
                local sub_tests
                sub_tests=$(awk -F, -v b="$bench" -v s="$scale" \
                    '$3==b && $5==s && NR>1 {print $4}' "$csv_file" | awk '!seen[$0]++')

                for sub_test in $sub_tests; do
                    # Get unit
                    local unit
                    unit=$(awk -F, -v b="$bench" -v s="$scale" -v st="$sub_test" \
                        '$3==b && $5==s && $4==st && NR>1 {print $9; exit}' "$csv_file")

                    # Collect values per scenario
                    local base_vals off_vals on_vals
                    base_vals=$(awk -F, -v b="$bench" -v s="$scale" -v st="$sub_test" \
                        '$3==b && $5==s && $4==st && $2=="baseline" && NR>1 {print $8}' "$csv_file")
                    off_vals=$(awk -F, -v b="$bench" -v s="$scale" -v st="$sub_test" \
                        '$3==b && $5==s && $4==st && $2=="undo_off" && NR>1 {print $8}' "$csv_file")
                    on_vals=$(awk -F, -v b="$bench" -v s="$scale" -v st="$sub_test" \
                        '$3==b && $5==s && $4==st && $2=="undo_on" && NR>1 {print $8}' "$csv_file")

                    # Compute medians via sort + awk
                    local base_med off_med on_med
                    base_med=$(echo "$base_vals" | sort -n | awk '{a[NR]=$1} END{if(NR==0)print "N/A"; else if(NR%2==1)print a[int(NR/2)+1]; else printf "%.2f",(a[NR/2]+a[NR/2+1])/2}')
                    off_med=$(echo "$off_vals" | sort -n | awk '{a[NR]=$1} END{if(NR==0)print "N/A"; else if(NR%2==1)print a[int(NR/2)+1]; else printf "%.2f",(a[NR/2]+a[NR/2+1])/2}')
                    on_med=$(echo "$on_vals" | sort -n | awk '{a[NR]=$1} END{if(NR==0)print "N/A"; else if(NR%2==1)print a[int(NR/2)+1]; else printf "%.2f",(a[NR/2]+a[NR/2+1])/2}')

                    # Compute ratios
                    local off_ratio on_ratio
                    off_ratio=$(echo "$off_med $base_med" | awk '{
                        if ($1=="N/A" || $2=="N/A" || $2+0==0) print "N/A"
                        else printf "%.2fx", $1/$2
                    }')
                    on_ratio=$(echo "$on_med $base_med" | awk '{
                        if ($1=="N/A" || $2=="N/A" || $2+0==0) print "N/A"
                        else printf "%.2fx", $1/$2
                    }')

                    # Compute CV% for the undo_on scenario (stability indicator)
                    local on_cv="N/A"
                    if [ -n "$on_vals" ] && [ "$(echo "$on_vals" | wc -w)" -ge 2 ]; then
                        on_cv=$(echo "$on_vals" | awk '
                        {a[NR]=$1; s+=$1}
                        END {
                            if (NR < 2) {print "N/A"; exit}
                            avg = s / NR
                            if (avg == 0) {print "0.0"; exit}
                            for (i=1; i<=NR; i++) ss += (a[i] - avg)^2
                            sd = sqrt(ss / (NR - 1))
                            printf "%.1f", (sd / avg) * 100
                        }')
                    fi

                    # Format values with unit
                    local base_fmt off_fmt on_fmt
                    if [ "$base_med" = "N/A" ]; then base_fmt="N/A"
                    else base_fmt=$(printf "%.1f %s" "$base_med" "$unit"); fi
                    if [ "$off_med" = "N/A" ]; then off_fmt="N/A"
                    else off_fmt=$(printf "%.1f %s" "$off_med" "$unit"); fi
                    if [ "$on_med" = "N/A" ]; then on_fmt="N/A"
                    else on_fmt=$(printf "%.1f %s" "$on_med" "$unit"); fi

                    printf " %-24s | %10s | %10s | %10s | %8s | %8s | %5s\n" \
                        "$sub_test" "$base_fmt" "$off_fmt" "$on_fmt" "$off_ratio" "$on_ratio" "$on_cv"
                done
                echo ""
            done
        done

        echo " ================================================================"
        echo " KEY FINDINGS (review after running on target hardware):"
        echo " - Code-presence overhead (UNDO Off vs Baseline): compare Off/Base columns"
        echo " - Per-table UNDO overhead: compare On/Base columns for B1-B3"
        echo " - Rollback cost vs cleanup benefit: B5 On/Base vs B6 On/Base"
        echo " - Read stability under writes: B4 On/Base post-update"
        echo " - Storage tradeoffs: B7 table sizes + UNDO log sizes"
        echo " ================================================================"
        echo ""
        echo " Full CSV data: $csv_file"
    } > "$output_file"

    cat "$output_file"
}
