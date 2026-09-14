# [DO NOT MERGE] Two-way table-AM benchmark: HEAP vs FLUX

This directory is a development benchmark, NOT for upstream. It compares heap
and FLUX on a single-connection OLTP micro-workload (200k-row load, index
build, 100k-row update, indexed-column update, range select, and WAL volume
per 100k updates).

## Capability matrix (as of this commit)

| AM    | bulk load | index build | update | indexed-col UPDATE    | full OLTP bench |
|-------|-----------|-------------|--------|-----------------------|-----------------|
| heap  | yes       | yes         | yes    | new-TID (native)      | YES (baseline)  |
| flux  | yes       | yes         | yes    | in-place (stable TID) | YES             |

## Representative result (16 vCPU, 30G RAM, PG -O2 cassert, fsync=off, single conn)

| metric                | heap    | flux     |
|-----------------------|---------|----------|
| insert 200k rows      | 0.38 s  | 2.07 s   |
| build index           | 0.06 s  | 0.07 s   |
| update 100k rows      | 0.80 s  | 1.04 s   |
| indexed-col update 4k | 0.06 s  | 0.14 s   |
| WAL / 100k updates    | 32 MB   | 863 MB   |

### Reading the numbers
- The headline is **WAL amplification**: FLUX writes far more WAL per update
  than heap because each in-place UPDATE durably logs the before-image to UNDO
  (heap does not), about 27x heap on this workload.
- heap remains faster and far lighter on WAL for this single-connection load;
  FLUX's value proposition (bloat control, in-place indexed UPDATE, stable
  TIDs) is not captured by this micro-benchmark and needs a bloat/space and
  concurrency study, which this harness does not yet do.

## Running
    src/test/benchmarks/am4way/bench4.sh      # writes ~/bench4_results.txt
The Python suite under suite/ is included for reference and further extension
(pgbench/HammerDB-style drivers); it was not re-validated against FLUX in this
commit.

## Do not merge
Marked [DO NOT MERGE]. Development artifact for comparing the AMs during
bring-up; not upstream-quality.
