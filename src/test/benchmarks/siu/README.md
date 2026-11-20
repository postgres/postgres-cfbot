# Selective index updates (SIU) A/B benchmark harness

Two postgres variants, identical pgdata layouts, pgbench workloads
exercising classic HOT, non-HOT, and selective-index-update (HOT-indexed)
paths.

Runs alternate the two variants at every iteration and repeat, sharing one
pgbench random seed per iteration between the arms; report the per-workload
median across iterations, not a single run.  Run the A/A control first
(`AA=1 ./scripts/run.sh`) to learn this host's run-to-run noise floor before
trusting any A/B delta.

## Contents

- `scripts/build.sh`: builds two postgres variants (`master` = tepid's
  merge-base with origin/master; `tepid` = the branch under test).  Requires
  a writable benchmark root via `BENCH` (default `/scratch/tepid-bench`).
- `scripts/run.sh`: A/B driver.  For `ITERATIONS` iterations, alternates the
  two variants and runs `simple_update` (pgbench -N), `hot_indexed_update`,
  `hot_indexed_mixed`, `read_indexscan`, and `wide_N` for N in `$WIDE_STEPS`.
  Collects TPS, latency, WAL bytes, HOT and SIU update counts, pre/post heap
  and index size, peak CPU% and RSS.  Writes a per-run CSV plus a
  median/min/max/spread summary to `$BENCH/results/`.  `AA=1` runs the master
  build as both arms (the noise-floor control).
- `scripts/soak.sh`: long-running single-workload driver that samples
  TPS/HOT%/WAL/bloat every `$SAMPLE` seconds under `$DURATION` seconds
  of constant pressure, per variant.
- `scripts/bloat.sh`: single-variant bloat probe.  Runs update+vacuum cycles
  on a table whose changed column and an unchanged column are both indexed, and
  reports (via pgstattuple) that the changed index stays bounded with periodic
  VACUUM but grows unbounded without it, plus the skip count showing
  SIU updates avoiding the unchanged index.  Spins its own throwaway
  cluster, so it does not touch the A/B pgdata.
- `scripts/split_ab.sh`: isolates the cost of the per-block
  VISIBILITYMAP_LOCATOR_SPLIT signal alone, rather than the
  selective-maintenance win.  It builds two variants from the tepid tree that
  differ only in whether BitmapAnd honors the split bit (`split_on` vs
  `split_off`, the latter with heap's bitmap_and_inexact callback forced NULL),
  so the only difference measured is the union/recheck the signal triggers.
- `scripts/hot_indexed_update.sql`: `UPDATE siu_table SET b = rand WHERE a = rand`.
- `scripts/hot_indexed_mixed.sql`: 80 % SELECT by PK + 20 % indexed-col UPDATE.
- `scripts/bitmap_and_mixed.sql`: forces a BitmapAnd across two indexes
  that this workload never updates, so a fresh mid-chain entry on a third index
  unions their shared block under BitmapAnd; used with `split_ab.sh` to price
  the split-bit union/recheck.
- `scripts/read_indexscan.sql`: read-only btree index scans on a freshly
  reset `siu_table` (no stale entries); confirms the SIU read path
  adds no per-scan overhead, since the crossed-attribute bitmap decides
  staleness without a key comparison and the scan requests no index tuple.
- `scripts/wide_update.sql`: driver script for the wide-table workload;
  the `SET` clause is built at run time from `$WIDE_STEPS`.

## Running

```
# Build both variants (run once per benchmark host)
REPO=$HOME/ws/postgres/tepid BENCH=/scratch/tepid-bench \
  ./scripts/build.sh

# A/A control first: establish the noise floor (both arms are master)
AA=1 SCALE=20 CLIENTS=16 THREADS=8 DURATION=120 ITERATIONS=5 \
  ./scripts/run.sh

# Standard A/B
SCALE=20 CLIENTS=16 THREADS=8 DURATION=120 ITERATIONS=5 \
  WIDE_COLS=16 WIDE_STEPS=0,1,2,4,8,16 \
  ./scripts/run.sh

# Soak
SCALE=50 CLIENTS=16 THREADS=8 DURATION=900 SAMPLE=60 \
  ./scripts/soak.sh

# Split-bit isolation (builds its own split_on/split_off variants)
REPO=$HOME/ws/postgres/tepid SCALE=10 CLIENTS=16 THREADS=8 \
  DURATION=60 ITERATIONS=5 ./scripts/split_ab.sh

# Bloat probe (single variant; defaults to the tepid build)
BENCH=/scratch/tepid-bench ROWS=5000 CYCLES=8 UPDATES=20 \
  ./scripts/bloat.sh
```

## Env vars

```
REPO         path to postgres source (has .git)
BENCH        bench root (install prefixes, build trees, results)
SCALE        pgbench -s (also drives siu_table row count = SCALE*100k)
CLIENTS      pgbench -c
THREADS      pgbench -j
DURATION     seconds per workload
ITERATIONS   A/B repetitions (default 5); report the median across them
SEED         base pgbench --random-seed; per-iteration if unset
AA           run.sh only: 1 runs the master build as both arms (noise floor)
WIDE_COLS    number of indexed int columns in wide_table (default 16)
WIDE_STEPS   comma-separated list of columns-modified values to exercise
             (default 0,1,4,8,16)
PORT         postgres port for the bench servers
SHARED_BUFFERS  postgresql.conf setting (default 512MB)
MASTER_REV   revision for the master variant (default: tepid's merge-base
             with origin/master)
TEPID_REV    revision for the tepid variant (default: tepid)

bloat.sh only:
BINDIR       postgres bin dir to test (default: tepid variant under $BENCH)
ROWS         seeded rows (default 5000)
CYCLES       update+vacuum cycles (default 8)
UPDATES      updates per row per cycle (default 20)
```

The scripts are portable between Linux and FreeBSD; the CPU/RSS sampler
uses `ps -o pcpu=,rss= --ppid LEADER -p LEADER` (Linux) or `pgrep -P` +
per-pid `ps` (FreeBSD); peak values are approximate.
