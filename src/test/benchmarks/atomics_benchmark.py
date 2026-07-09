"""
Main benchmark orchestrator for comparing traditional vs stdatomic.h implementations.

This module coordinates:
- PostgreSQL instance lifecycle management
- Schema creation and data loading
- Workload execution on both implementations
- Results collection and comparison
"""

import asyncio
import asyncpg
import logging
import os
import shutil
import signal
import subprocess
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from .atomics_config import (
    AtomicsBenchmarkConfig,
    DataDistribution,
    PostgresInstance,
    QueryPattern,
    TableSchema,
)
from .data_generator import DataGenerator
from .atomics_workload import AtomicsWorkloadRunner, WorkloadResult

logger = logging.getLogger(__name__)


@dataclass
class BenchmarkMetrics:
    """Collected metrics for a benchmark run."""
    implementation: str  # "traditional" or "stdatomic"
    schema_name: str
    row_count: int
    distribution: str
    load_time_seconds: float
    table_size_bytes: int
    index_size_bytes: int
    shared_buffers_hit_ratio: float = 0.0
    cache_hit_ratio: float = 0.0


@dataclass
class ComparisonResult:
    """Result of comparing traditional vs stdatomic for one scenario."""
    schema_name: str
    row_count: int
    distribution: str
    pattern: str
    traditional_ms: float
    stdatomic_ms: float
    speedup: float  # stdatomic_ms / traditional_ms (< 1.0 means faster)
    traditional_metrics: BenchmarkMetrics
    stdatomic_metrics: BenchmarkMetrics


class PostgresManager:
    """Manages PostgreSQL instance lifecycle."""

    def __init__(self, instance: PostgresInstance):
        self.instance = instance
        self.process: Optional[subprocess.Popen] = None
        self.pool: Optional[asyncpg.Pool] = None

    async def initialize_cluster(self):
        """Initialize a new database cluster if needed."""
        if self.instance.data_dir.exists():
            logger.info(
                "Data directory %s exists, skipping initdb", self.instance.data_dir
            )
            return

        logger.info("Initializing database cluster at %s", self.instance.data_dir)
        self.instance.data_dir.mkdir(parents=True, exist_ok=True)

        # For meson build directories (not installed), create symlink for postgres binary
        # in initdb directory so initdb can find it
        if not self.instance._is_install_dir():
            initdb_dir = self.instance.initdb_bin.parent
            postgres_link = initdb_dir / "postgres"
            if not postgres_link.exists():
                postgres_link.symlink_to(self.instance.postgres_bin)
                logger.debug("Created postgres symlink: %s -> %s", postgres_link, self.instance.postgres_bin)

        cmd = [
            str(self.instance.initdb_bin),
            "-D",
            str(self.instance.data_dir),
            "--no-locale",
            "--encoding=UTF8",
        ]

        # Set LD_LIBRARY_PATH for installed directories
        env = None
        if self.instance._is_install_dir():
            lib_dir = self.instance.build_dir / "lib64"
            env = os.environ.copy()
            env["LD_LIBRARY_PATH"] = f"{lib_dir}:{env.get('LD_LIBRARY_PATH', '')}"
            logger.debug("Setting LD_LIBRARY_PATH=%s", env["LD_LIBRARY_PATH"])

        result = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"initdb failed: {result.stderr}")

        logger.info("Database cluster initialized successfully")

        # Configure for benchmarking
        self._configure_for_performance()

    def _configure_for_performance(self):
        """Update postgresql.conf for benchmark performance."""
        conf_path = self.instance.data_dir / "postgresql.conf"

        settings = {
            "shared_buffers": "1GB",
            "effective_cache_size": "4GB",
            "maintenance_work_mem": "256MB",
            "checkpoint_completion_target": "0.9",
            "wal_buffers": "16MB",
            "default_statistics_target": "100",
            "random_page_cost": "1.1",
            "effective_io_concurrency": "200",
            "work_mem": "32MB",
            "min_wal_size": "1GB",
            "max_wal_size": "4GB",
            "max_worker_processes": "8",
            "max_parallel_workers_per_gather": "4",
            "max_parallel_workers": "8",
            "max_parallel_maintenance_workers": "4",
            # Enable timing
            "track_activities": "on",
            "track_counts": "on",
            "track_io_timing": "on",
            "track_functions": "all",
            # Reduce checkpointing during benchmark
            "checkpoint_timeout": "30min",
        }

        with open(conf_path, "a") as f:
            f.write("\n\n# Performance settings for benchmarking\n")
            for key, value in settings.items():
                f.write(f"{key} = {value}\n")

        logger.info("Configured PostgreSQL for performance benchmarking")

    async def start(self):
        """Start the PostgreSQL instance."""
        if self.process is not None:
            logger.warning("PostgreSQL is already running")
            return

        logger.info(
            "Starting PostgreSQL (%s) on port %d",
            self.instance.name,
            self.instance.port,
        )

        cmd = [
            str(self.instance.postgres_bin),
            "-D",
            str(self.instance.data_dir),
            "-p",
            str(self.instance.port),
            "-c",
            "logging_collector=off",
        ]

        # Set LD_LIBRARY_PATH for installed directories
        env = None
        if self.instance._is_install_dir():
            lib_dir = self.instance.build_dir / "lib64"
            env = os.environ.copy()
            env["LD_LIBRARY_PATH"] = f"{lib_dir}:{env.get('LD_LIBRARY_PATH', '')}"

        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )

        # Wait for server to be ready
        await self._wait_for_ready()

        # Create connection pool
        await self._create_pool()

        logger.info("PostgreSQL (%s) started successfully", self.instance.name)

    async def _wait_for_ready(self, timeout: int = 30):
        """Wait for PostgreSQL to be ready to accept connections."""
        start = time.time()
        while time.time() - start < timeout:
            try:
                conn = await asyncpg.connect(
                    host=self.instance.host,
                    port=self.instance.port,
                    user=self.instance.user or os.getenv("USER"),
                    database="postgres",
                    timeout=1,
                )
                await conn.close()
                return
            except (asyncpg.CannotConnectNowError, OSError):
                await asyncio.sleep(0.5)

        raise RuntimeError(
            f"PostgreSQL ({self.instance.name}) did not start within {timeout}s"
        )

    async def _create_pool(self):
        """Create connection pool."""
        self.pool = await asyncpg.create_pool(
            host=self.instance.host,
            port=self.instance.port,
            user=self.instance.user or os.getenv("USER"),
            database="postgres",
            min_size=2,
            max_size=10,
        )

    async def stop(self):
        """Stop the PostgreSQL instance."""
        if self.process is None:
            return

        logger.info("Stopping PostgreSQL (%s)", self.instance.name)

        if self.pool:
            await self.pool.close()
            self.pool = None

        # Send SIGTERM
        self.process.terminate()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            logger.warning("PostgreSQL did not stop gracefully, sending SIGKILL")
            self.process.kill()
            self.process.wait()

        self.process = None
        logger.info("PostgreSQL (%s) stopped", self.instance.name)

    async def execute(self, query: str):
        """Execute a query."""
        async with self.pool.acquire() as conn:
            return await conn.execute(query)

    async def fetch(self, query: str):
        """Fetch query results."""
        async with self.pool.acquire() as conn:
            return await conn.fetch(query)

    async def fetchrow(self, query: str):
        """Fetch single row."""
        async with self.pool.acquire() as conn:
            return await conn.fetchrow(query)


class AtomicsBenchmarkSuite:
    """Main benchmark orchestrator."""

    def __init__(self, config: Optional[AtomicsBenchmarkConfig] = None):
        self.config = config or AtomicsBenchmarkConfig()

        self.traditional_mgr = PostgresManager(self.config.traditional_instance)
        self.stdatomic_mgr = PostgresManager(self.config.stdatomic_instance)

        self.data_generator = DataGenerator(seed=self.config.seed)
        self.results: List[ComparisonResult] = []

    async def setup(self):
        """Initialize both PostgreSQL instances."""
        logger.info("Setting up benchmark environment...")

        # Initialize clusters
        await self.traditional_mgr.initialize_cluster()
        await self.stdatomic_mgr.initialize_cluster()

        # Start instances
        await self.traditional_mgr.start()
        await self.stdatomic_mgr.start()

        # Create benchmark database
        await self.traditional_mgr.execute("CREATE DATABASE benchmark_db")
        await self.stdatomic_mgr.execute("CREATE DATABASE benchmark_db")

        logger.info("Benchmark environment ready")

    async def teardown(self):
        """Stop both PostgreSQL instances."""
        logger.info("Tearing down benchmark environment...")
        await self.traditional_mgr.stop()
        await self.stdatomic_mgr.stop()

    async def run_benchmark(
        self,
        schema: TableSchema,
        row_count: int,
        distribution: DataDistribution,
    ) -> List[ComparisonResult]:
        """Run complete benchmark for one scenario, comparing both implementations."""
        logger.info(
            "=== Benchmark: %s, %d rows, %s ===",
            schema.name,
            row_count,
            distribution.value,
        )

        results = []

        # Run on traditional
        trad_workload, trad_metrics = await self._run_on_instance(
            self.traditional_mgr, "traditional", schema, row_count, distribution
        )

        # Run on stdatomic
        std_workload, std_metrics = await self._run_on_instance(
            self.stdatomic_mgr, "stdatomic", schema, row_count, distribution
        )

        # Compare results
        for trad_result in trad_workload.results:
            pattern = trad_result.query_pattern
            std_result = next(
                (r for r in std_workload.results if r.query_pattern == pattern), None
            )
            if std_result is None:
                continue

            speedup = trad_result.elapsed_seconds / std_result.elapsed_seconds

            comparison = ComparisonResult(
                schema_name=schema.name,
                row_count=row_count,
                distribution=distribution.value,
                pattern=pattern,
                traditional_ms=trad_result.elapsed_seconds * 1000,
                stdatomic_ms=std_result.elapsed_seconds * 1000,
                speedup=speedup,
                traditional_metrics=trad_metrics,
                stdatomic_metrics=std_metrics,
            )
            results.append(comparison)

        return results

    async def _run_on_instance(
        self,
        mgr: PostgresManager,
        impl_name: str,
        schema: TableSchema,
        row_count: int,
        distribution: DataDistribution,
    ) -> Tuple[WorkloadResult, BenchmarkMetrics]:
        """Run benchmark on a single PostgreSQL instance."""
        # Create table
        table_name = f"{schema.name}_{distribution.value}"
        await self._create_table(mgr, table_name, schema)

        # Load data
        load_start = time.perf_counter()
        insert_sql = self.data_generator.generate_server_side_insert(
            schema, row_count, distribution, table_suffix=f"_{distribution.value}"
        )
        await mgr.execute(insert_sql)
        load_time = time.perf_counter() - load_start

        logger.info("%s: loaded %d rows in %.2fs", impl_name, row_count, load_time)

        # Collect pre-workload metrics
        metrics = await self._collect_metrics(
            mgr, impl_name, table_name, schema.name, row_count, distribution.value
        )
        metrics.load_time_seconds = load_time

        # Run workload
        runner = AtomicsWorkloadRunner(
            mgr.pool,
            warmup_iterations=self.config.warmup_iterations,
            measure_iterations=self.config.measure_iterations,
        )

        workload_result = await runner.run_workload(
            schema=schema,
            table_name=table_name,
            row_count=row_count,
            distribution=distribution.value,
            patterns=self.config.query_patterns,
        )

        # Cleanup
        await mgr.execute(f"DROP TABLE IF EXISTS {table_name} CASCADE")

        return workload_result, metrics

    async def _create_table(
        self, mgr: PostgresManager, table_name: str, schema: TableSchema
    ):
        """Create a table with the given schema."""
        col_defs = []
        for col_name, col_type in schema.columns:
            col_defs.append(f"{col_name} {col_type.value}")

        create_sql = f"CREATE TABLE {table_name} ({', '.join(col_defs)})"
        await mgr.execute(create_sql)

        # Create indexes
        for idx_col in schema.index_columns:
            idx_name = f"{table_name}_{idx_col}_idx"
            await mgr.execute(
                f"CREATE INDEX {idx_name} ON {table_name} ({idx_col})"
            )

    async def _collect_metrics(
        self,
        mgr: PostgresManager,
        impl_name: str,
        table_name: str,
        schema_name: str,
        row_count: int,
        distribution: str,
    ) -> BenchmarkMetrics:
        """Collect metrics for a table."""
        # Table size
        size_row = await mgr.fetchrow(
            f"SELECT pg_total_relation_size('{table_name}') as total_size"
        )
        table_size = size_row["total_size"]

        # Index size
        idx_size_row = await mgr.fetchrow(
            f"SELECT pg_indexes_size('{table_name}') as idx_size"
        )
        index_size = idx_size_row["idx_size"]

        return BenchmarkMetrics(
            implementation=impl_name,
            schema_name=schema_name,
            row_count=row_count,
            distribution=distribution,
            load_time_seconds=0.0,  # Set by caller
            table_size_bytes=table_size,
            index_size_bytes=index_size,
        )

    async def run_full_suite(self) -> Dict:
        """Run complete benchmark suite."""
        start_time = time.perf_counter()
        all_results = []

        for schema in self.config.schemas:
            for row_count in self.config.get_row_counts():
                for distribution in self.config.distributions:
                    try:
                        results = await self.run_benchmark(
                            schema, row_count, distribution
                        )
                        all_results.extend(results)
                    except Exception as e:
                        logger.error(
                            "Benchmark failed for %s/%d/%s: %s",
                            schema.name,
                            row_count,
                            distribution.value,
                            e,
                            exc_info=True,
                        )

        elapsed_time = time.perf_counter() - start_time

        # Generate summary
        summary = self._generate_summary(all_results, elapsed_time)
        self.results = all_results

        return summary

    def _generate_summary(
        self, results: List[ComparisonResult], elapsed_time: float
    ) -> Dict:
        """Generate summary statistics."""
        if not results:
            return {}

        speedups = [r.speedup for r in results]

        summary = {
            "total_scenarios": len(results),
            "elapsed_time_seconds": elapsed_time,
            "median_speedup": sorted(speedups)[len(speedups) // 2],
            "mean_speedup": sum(speedups) / len(speedups),
            "min_speedup": min(speedups),
            "max_speedup": max(speedups),
            "stdatomic_faster_count": sum(1 for s in speedups if s > 1.0),
            "traditional_faster_count": sum(1 for s in speedups if s < 1.0),
            "neutral_count": sum(1 for s in speedups if s == 1.0),
        }

        # Per-pattern breakdown
        pattern_speedups = {}
        for result in results:
            if result.pattern not in pattern_speedups:
                pattern_speedups[result.pattern] = []
            pattern_speedups[result.pattern].append(result.speedup)

        summary["per_pattern_avg_speedup"] = {
            pattern: sum(speedups) / len(speedups)
            for pattern, speedups in pattern_speedups.items()
        }

        # Find best/worst scenarios
        best = max(results, key=lambda r: r.speedup)
        worst = min(results, key=lambda r: r.speedup)

        summary["best_stdatomic_scenario"] = {
            "pattern": best.pattern,
            "schema": best.schema_name,
            "distribution": best.distribution,
            "speedup": best.speedup,
        }

        summary["worst_stdatomic_scenario"] = {
            "pattern": worst.pattern,
            "schema": worst.schema_name,
            "distribution": worst.distribution,
            "speedup": worst.speedup,
        }

        return summary


async def run_atomics_benchmark(
    config: Optional[AtomicsBenchmarkConfig] = None,
) -> Dict:
    """Main entry point for running atomics benchmark."""
    suite = AtomicsBenchmarkSuite(config)

    try:
        await suite.setup()
        summary = await suite.run_full_suite()
        return {"summary": summary, "results": suite.results}
    finally:
        await suite.teardown()
