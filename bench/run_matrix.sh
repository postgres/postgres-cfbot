#!/bin/bash
#
# Run lazy_snapshot_bench.sh across a matrix of N values and emit one CSV.
#
# This script runs against a SINGLE PG cluster (it does not switch binaries).
# To compare master vs patch, run this twice with the appropriate cluster
# running each time, then concat the two CSVs.
#
# Example:
#   # Cluster is built from master code
#   $0 master 500 1000 2000 5000 > master.csv
#
#   # ... rebuild with patch applied, restart cluster ...
#   $0 patch 500 1000 2000 5000 > patch.csv
#
#   # combine
#   cat master.csv patch.csv > both.csv
#
# Each row is run REPEAT times (default 3) and all replicates are output.
# Aggregate (min/median/max) externally with awk/Python/pandas.

set -euo pipefail

REPEAT=${REPEAT:-3}
K=${K:-1}
LDWM=${LDWM:-64kB}

HERE="$(cd "$(dirname "$0")" && pwd)"
BENCH="$HERE/lazy_snapshot_bench.sh"

if [[ $# -lt 2 ]]; then
  cat <<EOF >&2
Usage: $0 <label> <N> [<N> ...]
  label: "master" | "patch" | any tag
  N:     one or more values to test
Environment:
  REPEAT (default 3) — runs per (label, N) combo
  K      (default 1) — inserts in long txn
  LDWM   (default 64kB) — logical_decoding_work_mem (small to amplify spill)
EOF
  exit 1
fi

LABEL="$1"
shift
N_VALUES=("$@")

# header
echo "label,N,K,ldwm,decoding_ms,decoded_changes,spill_txns,spill_count,spill_bytes,total_bytes,ddl_loop_sec,iter"

for N in "${N_VALUES[@]}"; do
  for ((i = 1; i <= REPEAT; i++)); do
    echo "==> $LABEL N=$N iter=$i/$REPEAT" >&2
    row=$("$BENCH" -N "$N" -K "$K" -m "$LDWM" -l "$LABEL")
    echo "$row,$i"
  done
done
