"""
Main orchestrator: coordinates data generation, schema creation, workload
execution, metrics collection, analysis, and visualization for the full
benchmark matrix.
"""

import asyncio
import logging
import os
import time
from datetime import datetime
from typing import List, Optional, Tuple

from .config import (
    ALL_SCHEMAS,
    BenchmarkConfig,
    DataDistribution,
    QueryPattern,
    TableSchema,
)
from .data_generator import DataGenerator
from .database import DatabaseManager
from .metrics_collector import BenchmarkMetrics, MetricsCollector
from .result_analyzer import AnalysisReport, ResultAnalyzer
from .schema_builder import SchemaBuilder
from .visualizer import Visualizer
from .workload_runner import WorkloadResult, WorkloadRunner

logger = logging.getLogger(__name__)


class BenchmarkSuite:
    """Orchestrates the full Noxu benchmark suite."""

    def __init__(self, config: Optional[BenchmarkConfig] = None):
        self.config = config or BenchmarkConfig()
        self.db = DatabaseManager(self.config.connection)
        self.schema_builder = SchemaBuilder(self.db)
        self.data_generator = DataGenerator(seed=self.config.seed)
        self.workload_runner = WorkloadRunner(
            self.db,
            warmup_iterations=self.config.warmup_iterations,
            measure_iterations=self.config.measure_iterations,
        )
        self.metrics_collector = MetricsCollector(self.db)
        self.analyzer = ResultAnalyzer()

        # Collected results
        self._workload_pairs: List[Tuple[WorkloadResult, WorkloadResult]] = []
        self._metrics_list: List[BenchmarkMetrics] = []

    async def setup(self):
        """Initialize database connections and verify Noxu availability."""
        logger.info("Initializing benchmark suite...")
        await self.db.initialize()

        # Check Noxu
        if not await self.db.check_noxu_available():
            raise RuntimeError(
                "Noxu table AM not found. Ensure PostgreSQL is built with Noxu support."
            )
        logger.info("Noxu table AM is available")

        # Try to enable pg_stat_statements
        if self.config.enable_pg_stat_statements:
            ok = await self.db.ensure_extension("pg_stat_statements")
            if not ok:
                logger.warning(
                    "pg_stat_statements not available; some metrics will be missing"
                )
                self.config.enable_pg_stat_statements = False

    async def teardown(self):
        """Close database connections."""
        await self.db.close()

    async def run_single_benchmark(
        self,
        schema: TableSchema,
        row_count: int,
        distribution: DataDistribution,
    ) -> Tuple[WorkloadResult, WorkloadResult, BenchmarkMetrics]:
        """Run a complete benchmark for one (schema, row_count, distribution) combination."""
        dist_name = distribution.value
        logger.info(
            "=== Benchmark: %s, %d rows, %s distribution ===",
            schema.name,
            row_count,
            dist_name,
        )

        # 1. Create tables
        tables = await self.schema_builder.setup_benchmark_tables(schema)
        heap_table = tables["heap_table"]
        noxu_table = tables["noxu_table"]

        # 2. Generate and load data
        insert_sql_heap = self.data_generator.generate_server_side_insert(
            schema, row_count, distribution, table_suffix="_heap"
        )
        insert_sql_noxu = self.data_generator.generate_server_side_insert(
            schema, row_count, distribution, table_suffix="_noxu"
        )

        logger.info("Loading %d rows into %s...", row_count, heap_table)
        t0 = time.perf_counter()
        await self.schema_builder.load_data(heap_table, insert_sql_heap)
        heap_load_time = time.perf_counter() - t0
        logger.info("HEAP load: %.2fs", heap_load_time)

        logger.info("Loading %d rows into %s...", row_count, noxu_table)
        t0 = time.perf_counter()
        await self.schema_builder.load_data(noxu_table, insert_sql_noxu)
        noxu_load_time = time.perf_counter() - t0
        logger.info("Noxu load: %.2fs", noxu_load_time)

        # 3. Reset stats
        if self.config.enable_pg_stat_statements:
            await self.db.reset_pg_stat_statements()

        # 4. Run workloads
        heap_wr, noxu_wr = await self.workload_runner.run_workload(
            schema=schema,
            heap_table=heap_table,
            noxu_table=noxu_table,
            row_count=row_count,
            distribution=dist_name,
            patterns=self.config.query_patterns,
        )

        # 5. Collect metrics
        metrics = await self.metrics_collector.collect_all(
            heap_table=heap_table,
            noxu_table=noxu_table,
            schema_name=schema.name,
            row_count=row_count,
            distribution=dist_name,
        )

        # 6. Cleanup tables
        await self.schema_builder.cleanup(schema)

        return heap_wr, noxu_wr, metrics

    async def run_full_suite(self) -> AnalysisReport:
        """Run the complete benchmark matrix and return an analysis report."""
        start_time = time.perf_counter()
        self._workload_pairs = []
        self._metrics_list = []

        total_combos = (
            len(self.config.schemas)
            * len(self.config.get_row_counts())
            * len(self.config.distributions)
        )
        combo_idx = 0

        for schema in self.config.schemas:
            for row_count in self.config.get_row_counts():
                for dist in self.config.distributions:
                    combo_idx += 1
                    logger.info(
                        "--- Combination %d/%d ---", combo_idx, total_combos
                    )
                    try:
                        heap_wr, noxu_wr, metrics = await self.run_single_benchmark(
                            schema, row_count, dist
                        )
                        self._workload_pairs.append((heap_wr, noxu_wr))
                        self._metrics_list.append(metrics)
                    except Exception as e:
                        logger.error(
                            "Benchmark failed for %s/%d/%s: %s",
                            schema.name,
                            row_count,
                            dist.value,
                            e,
                        )

        elapsed = time.perf_counter() - start_time
        logger.info("Full suite completed in %.1fs", elapsed)

        # Analyze
        report = self.analyzer.build_report(self._workload_pairs, self._metrics_list)
        return report

    def generate_output(self, report: AnalysisReport) -> str:
        """Generate CSV files, charts, and HTML dashboard.

        Returns the path to the output directory.
        """
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_dir = os.path.join(self.config.output_dir, f"run_{timestamp}")
        viz = Visualizer(output_dir)

        csv_path = viz.export_csv(report)
        logger.info("CSV results: %s", csv_path)

        dashboard_path = viz.generate_dashboard(report)
        logger.info("Dashboard: %s", dashboard_path)

        return output_dir


async def run_benchmark(config: Optional[BenchmarkConfig] = None) -> AnalysisReport:
    """Convenience entry point: run the full suite and generate output."""
    suite = BenchmarkSuite(config)
    try:
        await suite.setup()
        report = await suite.run_full_suite()
        output_dir = suite.generate_output(report)
        logger.info("Results written to: %s", output_dir)
        return report
    finally:
        await suite.teardown()
