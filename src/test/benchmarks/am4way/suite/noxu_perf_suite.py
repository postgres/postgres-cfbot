#!/usr/bin/env python3
"""
Noxu Performance Benchmark Suite

Comprehensive benchmarking framework for comparing Noxu columnar storage
against PostgreSQL's standard HEAP table access method.

This is the top-level entry point that orchestrates the full benchmark
pipeline:
  1. Configuration and connection setup
  2. Schema creation for HEAP and Noxu table pairs
  3. Reproducible data generation across multiple distributions
  4. Workload execution with warmup and measurement phases
  5. Metrics collection (pg_stat_statements, storage sizes, compression)
  6. Statistical analysis (mean, median, p95, p99, speedup ratios)
  7. Visualization (matplotlib charts + HTML dashboard with recommendations)
  8. CSV result export

Test Matrix:
  - Table shapes: narrow (4 cols), medium (11 cols), wide (55 cols)
  - Data types: int, bigint, text, boolean, uuid, timestamp, float, numeric, jsonb
  - Distributions: random, clustered, low_cardinality, high_null
  - Table sizes: 1K, 10K, 100K (default); up to 100M with --full-matrix
  - Query patterns: full_scan, column_projection, filtered_scan,
                    aggregation, group_by, index_scan

Usage:
    python noxu_perf_suite.py [OPTIONS]

    # Quick run with defaults
    python noxu_perf_suite.py

    # Custom database
    python noxu_perf_suite.py --database mydb --host localhost

    # Full matrix (all row counts up to 100M)
    python noxu_perf_suite.py --full-matrix

    # Specific schema and row count
    python noxu_perf_suite.py --schema wide --rows 100000 1000000

    # Specific distribution
    python noxu_perf_suite.py --distribution high_null

    # Verbose output with custom output directory
    python noxu_perf_suite.py -v --output-dir /tmp/noxu_bench

Environment Variables:
    PGHOST       PostgreSQL host (default: localhost)
    PGPORT       PostgreSQL port (default: 5432)
    PGDATABASE   Database name (default: benchmark_db)
    PGUSER       Database user
    PGPASSWORD   Database password
"""

import argparse
import asyncio
import logging
import os
import sys

# Allow running directly (python noxu_perf_suite.py) or as a module
# (python -m benchmarks.noxu_perf_suite). Ensure the parent of the
# benchmarks package is on sys.path so absolute imports work.
_pkg_dir = os.path.dirname(os.path.abspath(__file__))
_parent_dir = os.path.dirname(_pkg_dir)
if _parent_dir not in sys.path:
    sys.path.insert(0, _parent_dir)

from benchmarks.config import (
    ALL_SCHEMAS,
    BenchmarkConfig,
    ConnectionConfig,
    DataDistribution,
    MEDIUM_SCHEMA,
    NARROW_SCHEMA,
    QueryPattern,
    WIDE_SCHEMA,
)
from benchmarks.benchmark_suite import BenchmarkSuite, run_benchmark


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Noxu Performance Benchmark Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # Connection
    conn_group = parser.add_argument_group("connection")
    conn_group.add_argument("--host", default=None, help="PostgreSQL host (env: PGHOST)")
    conn_group.add_argument("--port", type=int, default=None, help="PostgreSQL port (env: PGPORT)")
    conn_group.add_argument("--database", "-d", default=None, help="Database name (env: PGDATABASE)")
    conn_group.add_argument("--user", "-U", default=None, help="Database user (env: PGUSER)")

    # Test matrix
    matrix_group = parser.add_argument_group("test matrix")
    matrix_group.add_argument(
        "--schema",
        choices=["narrow", "medium", "wide", "all"],
        default="all",
        help="Table schema to test (default: all)",
    )
    matrix_group.add_argument(
        "--rows",
        type=int,
        nargs="+",
        default=None,
        help="Row counts to test (default: 1000 10000 100000)",
    )
    matrix_group.add_argument(
        "--distribution",
        choices=["random", "clustered", "low_cardinality", "high_null", "all"],
        default="all",
        help="Data distribution (default: all)",
    )
    matrix_group.add_argument(
        "--pattern",
        choices=[p.value for p in QueryPattern] + ["all"],
        default="all",
        help="Query pattern to test (default: all)",
    )
    matrix_group.add_argument(
        "--full-matrix",
        action="store_true",
        help="Run full matrix including up to 100M rows",
    )

    # Execution
    exec_group = parser.add_argument_group("execution")
    exec_group.add_argument(
        "--warmup", type=int, default=2, help="Warmup iterations (default: 2)"
    )
    exec_group.add_argument(
        "--iterations", type=int, default=5, help="Measurement iterations (default: 5)"
    )
    exec_group.add_argument(
        "--seed", type=int, default=42, help="RNG seed for reproducibility (default: 42)"
    )

    # Output
    out_group = parser.add_argument_group("output")
    out_group.add_argument(
        "--output-dir", "-o", default="benchmark_results", help="Output directory"
    )
    out_group.add_argument(
        "-v", "--verbose", action="store_true", help="Verbose logging"
    )
    out_group.add_argument(
        "--json-summary", action="store_true",
        help="Print summary as JSON to stdout",
    )

    return parser.parse_args()


def build_config(args: argparse.Namespace) -> BenchmarkConfig:
    conn = ConnectionConfig()
    if args.host:
        conn.host = args.host
    if args.port:
        conn.port = args.port
    if args.database:
        conn.database = args.database
    if args.user:
        conn.user = args.user

    schema_map = {
        "narrow": [NARROW_SCHEMA],
        "medium": [MEDIUM_SCHEMA],
        "wide": [WIDE_SCHEMA],
        "all": list(ALL_SCHEMAS),
    }
    schemas = schema_map[args.schema]

    if args.distribution == "all":
        distributions = list(DataDistribution)
    else:
        distributions = [DataDistribution(args.distribution)]

    if args.pattern == "all":
        patterns = list(QueryPattern)
    else:
        patterns = [QueryPattern(args.pattern)]

    config = BenchmarkConfig(
        connection=conn,
        schemas=schemas,
        distributions=distributions,
        query_patterns=patterns,
        warmup_iterations=args.warmup,
        measure_iterations=args.iterations,
        seed=args.seed,
        output_dir=args.output_dir,
        full_matrix=args.full_matrix,
        verbose=args.verbose,
    )

    if args.rows:
        config.row_counts = args.rows

    return config


def print_banner(config: BenchmarkConfig):
    """Print the benchmark configuration banner."""
    total_combos = (
        len(config.schemas)
        * len(config.get_row_counts())
        * len(config.distributions)
    )
    total_queries = total_combos * len(config.query_patterns) * 2  # heap + noxu

    print("=" * 70)
    print("  Noxu Performance Benchmark Suite")
    print("=" * 70)
    print(f"  Database    : {config.connection.database} "
          f"({config.connection.host}:{config.connection.port})")
    print(f"  Schemas     : {[s.name for s in config.schemas]}")
    print(f"  Row counts  : {config.get_row_counts()}")
    print(f"  Distributions: {[d.value for d in config.distributions]}")
    print(f"  Patterns    : {[p.value for p in config.query_patterns]}")
    print(f"  Iterations  : {config.measure_iterations} "
          f"(warmup: {config.warmup_iterations})")
    print(f"  Total combos: {total_combos} "
          f"({total_queries} query executions)")
    print(f"  Output      : {config.output_dir}")
    print("=" * 70)
    print()


def print_results(report):
    """Print the results summary to stdout."""
    import json
    s = report.summary

    print()
    print("=" * 70)
    print("  RESULTS SUMMARY")
    print("=" * 70)
    if s.get("median_speedup"):
        print(f"  Median query speedup:      {s['median_speedup']:.2f}x")
        print(f"  Best speedup:              {s['max_speedup']:.2f}x")
        print(f"  Worst speedup:             {s['min_speedup']:.2f}x")
    if s.get("avg_compression_ratio"):
        print(f"  Avg compression ratio:     {s['avg_compression_ratio']:.2f}x")
        print(f"  Avg space savings:         {s.get('avg_space_savings_pct', 0):.1f}%")
    if s.get("per_pattern_avg_speedup"):
        print()
        print("  Per-pattern average speedup:")
        for pattern, speedup in sorted(s["per_pattern_avg_speedup"].items()):
            indicator = ">>>" if speedup > 1.0 else "   "
            print(f"    {indicator} {pattern:25s} {speedup:.2f}x")
    if s.get("best_noxu_scenario"):
        best = s["best_noxu_scenario"]
        print()
        print(
            f"  Best Noxu scenario: {best['pattern']} on {best['schema']} "
            f"({best['distribution']}) = {best['speedup']:.2f}x"
        )
    if s.get("worst_noxu_scenario"):
        worst = s["worst_noxu_scenario"]
        print(
            f"  Worst Noxu scenario: {worst['pattern']} on {worst['schema']} "
            f"({worst['distribution']}) = {worst['speedup']:.2f}x"
        )
    print("=" * 70)


def main():
    args = parse_args()

    log_level = logging.DEBUG if args.verbose else logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)-8s %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )

    config = build_config(args)
    print_banner(config)

    try:
        report = asyncio.run(run_benchmark(config))
    except KeyboardInterrupt:
        print("\nBenchmark interrupted.")
        sys.exit(1)
    except Exception as e:
        logging.error("Benchmark failed: %s", e, exc_info=True)
        sys.exit(1)

    print_results(report)

    if args.json_summary:
        import json
        print()
        print("JSON Summary:")
        print(json.dumps(report.summary, indent=2, default=str))


if __name__ == "__main__":
    main()
