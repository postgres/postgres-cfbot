"""
CLI entry point for atomics benchmark suite.

Usage:
    python -m src.test.benchmarks.atomics_cli [OPTIONS]

Examples:
    # Quick run with defaults
    python -m src.test.benchmarks.atomics_cli

    # Full matrix
    python -m src.test.benchmarks.atomics_cli --full-matrix

    # Specific schema and row count
    python -m src.test.benchmarks.atomics_cli --schema medium --rows 100000

    # Custom build directories
    python -m src.test.benchmarks.atomics_cli \\
        --traditional-build ./build-traditional \\
        --stdatomic-build ./build-stdatomic

    # Verbose output
    python -m src.test.benchmarks.atomics_cli -v
"""

import argparse
import asyncio
import json
import logging
import sys
from pathlib import Path

from .atomics_config import (
    ALL_SCHEMAS,
    AtomicsBenchmarkConfig,
    DataDistribution,
    MEDIUM_SCHEMA,
    NARROW_SCHEMA,
    PostgresInstance,
    QueryPattern,
    WIDE_SCHEMA,
)
from .atomics_benchmark import run_atomics_benchmark


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="PostgreSQL Atomics Performance Benchmark Suite",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )

    # Build directories
    parser.add_argument(
        "--traditional-build",
        type=Path,
        default=Path.cwd() / "build-traditional",
        help="Path to traditional atomics build directory",
    )
    parser.add_argument(
        "--stdatomic-build",
        type=Path,
        default=Path.cwd() / "build-stdatomic",
        help="Path to stdatomic.h build directory",
    )

    # PostgreSQL instance configuration
    parser.add_argument(
        "--traditional-port", type=int, default=5433, help="Port for traditional instance"
    )
    parser.add_argument(
        "--stdatomic-port", type=int, default=5434, help="Port for stdatomic instance"
    )
    parser.add_argument(
        "--traditional-datadir",
        type=Path,
        default=Path("/tmp/pgdata-traditional"),
        help="Data directory for traditional instance",
    )
    parser.add_argument(
        "--stdatomic-datadir",
        type=Path,
        default=Path("/tmp/pgdata-stdatomic"),
        help="Data directory for stdatomic instance",
    )

    # Test matrix
    parser.add_argument(
        "--schema",
        choices=["narrow", "medium", "wide", "all"],
        default="all",
        help="Table schema to test (default: all)",
    )
    parser.add_argument(
        "--rows",
        type=int,
        nargs="+",
        default=None,
        help="Row counts to test (default: 10000 100000 1000000)",
    )
    parser.add_argument(
        "--distribution",
        choices=["random", "clustered", "low_cardinality", "high_null", "all"],
        default="all",
        help="Data distribution (default: all)",
    )
    parser.add_argument(
        "--pattern",
        choices=[p.value for p in QueryPattern] + ["all"],
        default="all",
        help="Query pattern to test (default: all)",
    )
    parser.add_argument(
        "--full-matrix",
        action="store_true",
        help="Run full matrix including large row counts",
    )

    # Execution
    parser.add_argument(
        "--warmup", type=int, default=3, help="Warmup iterations (default: 3)"
    )
    parser.add_argument(
        "--iterations", type=int, default=10, help="Measurement iterations (default: 10)"
    )
    parser.add_argument(
        "--concurrent-clients",
        type=int,
        default=8,
        help="Number of concurrent clients for write tests (default: 8)",
    )
    parser.add_argument("--seed", type=int, default=42, help="RNG seed (default: 42)")

    # Output
    parser.add_argument(
        "--output-dir",
        "-o",
        default="atomics_benchmark_results",
        help="Output directory",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Verbose logging"
    )

    return parser.parse_args()


def build_config(args: argparse.Namespace) -> AtomicsBenchmarkConfig:
    """Build configuration from command-line arguments."""
    traditional_inst = PostgresInstance(
        name="traditional",
        build_dir=args.traditional_build,
        port=args.traditional_port,
        data_dir=args.traditional_datadir,
    )

    stdatomic_inst = PostgresInstance(
        name="stdatomic",
        build_dir=args.stdatomic_build,
        port=args.stdatomic_port,
        data_dir=args.stdatomic_datadir,
    )

    schema_map = {
        "narrow": [NARROW_SCHEMA],
        "medium": [MEDIUM_SCHEMA],
        "wide": [WIDE_SCHEMA],
        "all": list(ALL_SCHEMAS),
    }
    schemas = schema_map[args.schema]

    if args.distribution == "all":
        distributions = [
            DataDistribution.RANDOM,
            DataDistribution.CLUSTERED,
            DataDistribution.LOW_CARDINALITY,
        ]
    else:
        distributions = [DataDistribution(args.distribution)]

    if args.pattern == "all":
        # Default patterns (exclude concurrent patterns for now)
        patterns = [
            QueryPattern.FULL_SCAN,
            QueryPattern.COLUMN_PROJECTION,
            QueryPattern.FILTERED_SCAN,
            QueryPattern.AGGREGATION,
            QueryPattern.GROUP_BY,
            QueryPattern.INDEX_SCAN,
        ]
    else:
        patterns = [QueryPattern(args.pattern)]

    config = AtomicsBenchmarkConfig(
        traditional_instance=traditional_inst,
        stdatomic_instance=stdatomic_inst,
        schemas=schemas,
        distributions=distributions,
        query_patterns=patterns,
        warmup_iterations=args.warmup,
        measure_iterations=args.iterations,
        concurrent_clients=args.concurrent_clients,
        seed=args.seed,
        output_dir=args.output_dir,
        full_matrix=args.full_matrix,
        verbose=args.verbose,
    )

    if args.rows:
        config.row_counts = args.rows

    return config


def print_summary(summary: dict):
    """Print benchmark summary."""
    print()
    print("=" * 70)
    print("  ATOMICS BENCHMARK RESULTS SUMMARY")
    print("=" * 70)

    if not summary:
        print("  No results to display")
        return

    print(f"  Total scenarios tested:    {summary.get('total_scenarios', 0)}")
    print(f"  Total time:                {summary.get('elapsed_time_seconds', 0):.1f}s")
    print()

    median = summary.get("median_speedup", 1.0)
    mean = summary.get("mean_speedup", 1.0)
    print(f"  Median speedup (std/trad): {median:.3f}x")
    print(f"  Mean speedup:              {mean:.3f}x")
    print(f"  Min speedup:               {summary.get('min_speedup', 1.0):.3f}x")
    print(f"  Max speedup:               {summary.get('max_speedup', 1.0):.3f}x")
    print()

    faster = summary.get("stdatomic_faster_count", 0)
    slower = summary.get("traditional_faster_count", 0)
    total = summary.get("total_scenarios", 1)

    print(f"  Stdatomic faster:          {faster} ({100*faster/total:.1f}%)")
    print(f"  Traditional faster:        {slower} ({100*slower/total:.1f}%)")

    if summary.get("per_pattern_avg_speedup"):
        print()
        print("  Per-pattern average speedup (stdatomic vs traditional):")
        for pattern, speedup in sorted(summary["per_pattern_avg_speedup"].items()):
            indicator = ">>>" if speedup > 1.05 else "   "
            indicator = "<<<" if speedup < 0.95 else indicator
            print(f"    {indicator} {pattern:30s} {speedup:.3f}x")

    if summary.get("best_stdatomic_scenario"):
        best = summary["best_stdatomic_scenario"]
        print()
        print(
            f"  Best stdatomic: {best['pattern']} on {best['schema']} "
            f"({best['distribution']}) = {best['speedup']:.3f}x"
        )

    if summary.get("worst_stdatomic_scenario"):
        worst = summary["worst_stdatomic_scenario"]
        print(
            f"  Worst stdatomic: {worst['pattern']} on {worst['schema']} "
            f"({worst['distribution']}) = {worst['speedup']:.3f}x"
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

    # Verify build directories exist
    if not args.traditional_build.exists():
        print(f"ERROR: Traditional build directory not found: {args.traditional_build}")
        print("Please build with: meson setup build-traditional -Duse_stdatomic=no")
        sys.exit(1)

    if not args.stdatomic_build.exists():
        print(f"ERROR: Stdatomic build directory not found: {args.stdatomic_build}")
        print("Please build with: meson setup build-stdatomic -Duse_stdatomic=yes")
        sys.exit(1)

    config = build_config(args)

    print("=" * 70)
    print("  PostgreSQL Atomics Performance Benchmark")
    print("=" * 70)
    print(f"  Traditional build: {config.traditional_instance.build_dir}")
    print(f"  Stdatomic build:   {config.stdatomic_instance.build_dir}")
    print(f"  Schemas:           {[s.name for s in config.schemas]}")
    print(f"  Row counts:        {config.get_row_counts()}")
    print(f"  Distributions:     {[d.value for d in config.distributions]}")
    print(f"  Patterns:          {[p.value for p in config.query_patterns]}")
    print(
        f"  Iterations:        {config.measure_iterations} "
        f"(warmup: {config.warmup_iterations})"
    )
    print(f"  Output:            {config.output_dir}")
    print("=" * 70)
    print()

    try:
        report = asyncio.run(run_atomics_benchmark(config))
    except KeyboardInterrupt:
        print("\nBenchmark interrupted.")
        sys.exit(1)
    except Exception as e:
        logging.error("Benchmark failed: %s", e, exc_info=True)
        sys.exit(1)

    # Print summary
    print_summary(report.get("summary", {}))

    # Save results
    output_dir = Path(config.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    results_file = output_dir / "results.json"
    with open(results_file, "w") as f:
        # Convert results to serializable format
        results_data = []
        for r in report.get("results", []):
            results_data.append(
                {
                    "schema": r.schema_name,
                    "row_count": r.row_count,
                    "distribution": r.distribution,
                    "pattern": r.pattern,
                    "traditional_ms": r.traditional_ms,
                    "stdatomic_ms": r.stdatomic_ms,
                    "speedup": r.speedup,
                }
            )

        json.dump(
            {"summary": report.get("summary", {}), "results": results_data},
            f,
            indent=2,
        )

    print(f"\nResults saved to: {results_file}")


if __name__ == "__main__":
    main()
