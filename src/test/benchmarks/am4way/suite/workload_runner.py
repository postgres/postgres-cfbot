"""
Workload runner: executes query patterns against HEAP and Noxu tables,
collecting timing and EXPLAIN ANALYZE data.
"""

import logging
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from .config import ColumnType, QueryPattern, TableSchema
from .database import DatabaseManager

logger = logging.getLogger(__name__)


@dataclass
class QueryResult:
    """Result of a single query execution."""
    query_pattern: str
    table_name: str
    storage_method: str  # "heap" or "noxu"
    query_sql: str
    elapsed_seconds: float
    row_count: int = 0
    explain_plan: Optional[Dict[str, Any]] = None


@dataclass
class WorkloadResult:
    """Aggregated results for a complete workload run."""
    schema_name: str
    row_count: int
    distribution: str
    storage_method: str
    results: List[QueryResult] = field(default_factory=list)

    def add(self, result: QueryResult):
        self.results.append(result)


class WorkloadRunner:
    """Generates and executes query workloads against benchmark tables."""

    def __init__(
        self,
        db: DatabaseManager,
        warmup_iterations: int = 2,
        measure_iterations: int = 5,
    ):
        self.db = db
        self.warmup_iterations = warmup_iterations
        self.measure_iterations = measure_iterations

    # ------------------------------------------------------------------
    # Query generators per pattern
    # ------------------------------------------------------------------

    def _full_scan_query(self, table_name: str, schema: TableSchema) -> str:
        return f"SELECT * FROM {table_name}"

    def _column_projection_query(self, table_name: str, schema: TableSchema) -> str:
        # Select first 2 non-id columns (or all if < 2)
        cols = [c[0] for c in schema.columns if c[0] != "id"][:2]
        if not cols:
            cols = [schema.columns[0][0]]
        return f"SELECT {', '.join(cols)} FROM {table_name}"

    def _filtered_scan_query(self, table_name: str, schema: TableSchema) -> str:
        # Find a suitable filter column
        for col_name, col_type in schema.columns:
            if col_type == ColumnType.INT and col_name != "id":
                return f"SELECT * FROM {table_name} WHERE {col_name} > 0"
            if col_type == ColumnType.BOOLEAN:
                return f"SELECT * FROM {table_name} WHERE {col_name} = TRUE"
        # Fallback: filter on id
        return f"SELECT * FROM {table_name} WHERE id > 0 AND id <= 1000"

    def _aggregation_query(self, table_name: str, schema: TableSchema) -> str:
        agg_exprs = []
        for col_name, col_type in schema.columns:
            if col_type in (ColumnType.INT, ColumnType.BIGINT, ColumnType.FLOAT, ColumnType.NUMERIC):
                agg_exprs.append(f"SUM({col_name})")
                agg_exprs.append(f"AVG({col_name})")
                if len(agg_exprs) >= 6:
                    break
        if not agg_exprs:
            agg_exprs = ["COUNT(*)"]
        return f"SELECT COUNT(*), {', '.join(agg_exprs)} FROM {table_name}"

    def _group_by_query(self, table_name: str, schema: TableSchema) -> str:
        # Find a good GROUP BY column (low-ish cardinality integer or boolean)
        group_col = None
        agg_col = None
        for col_name, col_type in schema.columns:
            if col_name == "id":
                continue
            if col_type in (ColumnType.INT, ColumnType.BOOLEAN) and group_col is None:
                group_col = col_name
            if col_type in (ColumnType.FLOAT, ColumnType.NUMERIC, ColumnType.INT, ColumnType.BIGINT) and agg_col is None:
                agg_col = col_name

        if group_col is None:
            group_col = schema.columns[0][0]
        if agg_col is None:
            agg_col = "id"

        return (
            f"SELECT {group_col}, COUNT(*), SUM({agg_col}), AVG({agg_col}) "
            f"FROM {table_name} GROUP BY {group_col}"
        )

    def _index_scan_query(self, table_name: str, schema: TableSchema) -> str:
        return f"SELECT * FROM {table_name} WHERE id = 42"

    def _get_query(
        self, pattern: QueryPattern, table_name: str, schema: TableSchema
    ) -> str:
        generators = {
            QueryPattern.FULL_SCAN: self._full_scan_query,
            QueryPattern.COLUMN_PROJECTION: self._column_projection_query,
            QueryPattern.FILTERED_SCAN: self._filtered_scan_query,
            QueryPattern.AGGREGATION: self._aggregation_query,
            QueryPattern.GROUP_BY: self._group_by_query,
            QueryPattern.INDEX_SCAN: self._index_scan_query,
        }
        gen = generators.get(pattern)
        if gen is None:
            raise ValueError(f"Unknown query pattern: {pattern}")
        return gen(table_name, schema)

    # ------------------------------------------------------------------
    # Execution
    # ------------------------------------------------------------------

    async def _run_single(
        self,
        query: str,
        pattern: QueryPattern,
        table_name: str,
        storage_method: str,
        collect_explain: bool = True,
    ) -> QueryResult:
        """Run a single query, returning timing and optional EXPLAIN data."""
        # Warm up
        for _ in range(self.warmup_iterations):
            await self.db.fetch(query)

        # Measure
        timings = []
        row_count = 0
        for _ in range(self.measure_iterations):
            rows, elapsed = await self.db.fetch_timed(query)
            timings.append(elapsed)
            row_count = len(rows)

        median_time = sorted(timings)[len(timings) // 2]

        # Collect EXPLAIN ANALYZE on one run
        explain_plan = None
        if collect_explain:
            try:
                explain_plan = await self.db.explain_analyze(query)
            except Exception as e:
                logger.warning("EXPLAIN ANALYZE failed for %s: %s", table_name, e)

        return QueryResult(
            query_pattern=pattern.value,
            table_name=table_name,
            storage_method=storage_method,
            query_sql=query,
            elapsed_seconds=median_time,
            row_count=row_count,
            explain_plan=explain_plan,
        )

    async def run_workload(
        self,
        schema: TableSchema,
        heap_table: str,
        noxu_table: str,
        row_count: int,
        distribution: str,
        patterns: Optional[List[QueryPattern]] = None,
        collect_explain: bool = True,
    ) -> tuple:
        """Run a full workload against both HEAP and Noxu tables.

        Returns (heap_workload_result, noxu_workload_result).
        """
        if patterns is None:
            patterns = list(QueryPattern)

        heap_result = WorkloadResult(
            schema_name=schema.name,
            row_count=row_count,
            distribution=distribution,
            storage_method="heap",
        )
        noxu_result = WorkloadResult(
            schema_name=schema.name,
            row_count=row_count,
            distribution=distribution,
            storage_method="noxu",
        )

        for pattern in patterns:
            logger.info(
                "Running %s on %s/%s (rows=%d, dist=%s)",
                pattern.value,
                heap_table,
                noxu_table,
                row_count,
                distribution,
            )

            # HEAP
            heap_query = self._get_query(pattern, heap_table, schema)
            heap_qr = await self._run_single(
                heap_query, pattern, heap_table, "heap", collect_explain
            )
            heap_result.add(heap_qr)

            # Noxu
            noxu_query = self._get_query(pattern, noxu_table, schema)
            noxu_qr = await self._run_single(
                noxu_query, pattern, noxu_table, "noxu", collect_explain
            )
            noxu_result.add(noxu_qr)

            speedup = (
                heap_qr.elapsed_seconds / noxu_qr.elapsed_seconds
                if noxu_qr.elapsed_seconds > 0
                else float("inf")
            )
            logger.info(
                "  %s: heap=%.4fs noxu=%.4fs speedup=%.2fx",
                pattern.value,
                heap_qr.elapsed_seconds,
                noxu_qr.elapsed_seconds,
                speedup,
            )

        return heap_result, noxu_result

    async def run_custom_query(
        self,
        query: str,
        table_name: str,
        storage_method: str,
        label: str = "custom",
        collect_explain: bool = True,
    ) -> QueryResult:
        """Run an arbitrary query with benchmarking instrumentation."""
        return await self._run_single(
            query,
            QueryPattern.FULL_SCAN,  # placeholder
            table_name,
            storage_method,
            collect_explain,
        )
