#!/bin/bash
#
# Aggregate the CSV from run_matrix.sh into a comparison table suitable for
# pasting into a pgsql-hackers email.
#
# Usage:
#   $0 <combined.csv>
# or
#   cat master.csv patch.csv | $0 -
#
# Outputs, per (N, label) combo: median of decoding_ms / spill_bytes /
# total_bytes across iterations. Then a side-by-side comparison of master
# vs patch.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <csv-file>  (or '-' for stdin)" >&2
  exit 1
fi

INPUT="$1"
[[ "$INPUT" == "-" ]] && INPUT=/dev/stdin

awk -F, '
$1 == "label" { next }  # skip any header row (works for concatenated CSVs)
{
  key = $1 "," $2  # label,N
  decoding[key, ++cnt_dec[key]] = $5
  spill[key, ++cnt_sp[key]]     = $9
  total[key, ++cnt_tot[key]]    = $10
  labels[$1] = 1
  ns[$2 + 0] = 1
  # remember K (assume constant) and max repeat seen
  k = $3
  ldwm = $4
  if (cnt_dec[key] > rep_max) rep_max = cnt_dec[key]
}
END {
  # median helper baked into END via re-sort per-key (small N, OK)
  for (key in cnt_dec) {
    n = cnt_dec[key]
    # collect into arr
    delete arr
    for (i = 1; i <= n; i++) arr[i] = decoding[key, i]
    asort(arr)
    if (n % 2) m_dec[key] = arr[(n+1)/2]
    else       m_dec[key] = (arr[n/2] + arr[n/2 + 1]) / 2

    delete arr
    for (i = 1; i <= n; i++) arr[i] = spill[key, i]
    asort(arr)
    if (n % 2) m_sp[key] = arr[(n+1)/2]
    else       m_sp[key] = (arr[n/2] + arr[n/2 + 1]) / 2

    delete arr
    for (i = 1; i <= n; i++) arr[i] = total[key, i]
    asort(arr)
    if (n % 2) m_tot[key] = arr[(n+1)/2]
    else       m_tot[key] = (arr[n/2] + arr[n/2 + 1]) / 2
  }

  # Sorted N values
  n_count = 0
  for (n in ns) sorted_ns[++n_count] = n
  for (i = 1; i <= n_count; i++)
    for (j = i + 1; j <= n_count; j++)
      if (sorted_ns[i] + 0 > sorted_ns[j] + 0) {
        t = sorted_ns[i]; sorted_ns[i] = sorted_ns[j]; sorted_ns[j] = t
      }

  printf "Config: K=%s, logical_decoding_work_mem=%s, REPEAT=%d (median)\n\n", k, ldwm, rep_max
  printf "%-8s %-7s %-14s %-14s %-14s\n", "label", "N", "decode_ms", "spill_bytes", "total_bytes"
  for (i = 1; i <= n_count; i++) {
    N = sorted_ns[i]
    for (lbl in labels) {
      key = lbl "," N
      if (key in m_dec)
        printf "%-8s %-7d %-14d %-14d %-14d\n", lbl, N, m_dec[key], m_sp[key], m_tot[key]
    }
  }

  # Side-by-side, if exactly master + patch are present
  if ("master" in labels && "patch" in labels) {
    printf "\n%-7s %-12s %-12s %-12s %-12s %-12s %-12s\n", \
      "N", "master_dec", "patch_dec", "speedup", "master_spill", "patch_spill", "saved_x"
    for (i = 1; i <= n_count; i++) {
      N = sorted_ns[i]
      mk = "master," N; pk = "patch," N
      if ((mk in m_dec) && (pk in m_dec)) {
        speedup = (m_dec[pk] > 0) ? m_dec[mk] / m_dec[pk] : 0
        saved   = (m_sp[pk] > 0) ? m_sp[mk] / m_sp[pk] : (m_sp[mk] > 0 ? 999 : 1)
        printf "%-7d %-12d %-12d %-12.2f %-12d %-12d %-12.2f\n", \
          N, m_dec[mk], m_dec[pk], speedup, m_sp[mk], m_sp[pk], saved
      }
    }
  }
}
' "$INPUT"
