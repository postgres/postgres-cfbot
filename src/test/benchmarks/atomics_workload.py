"""
Workload runner for atomics benchmarking.

Executes query patterns that stress different atomic operations:
- Buffer management (spinlocks)
- Tuple visibility (atomic reads)
- Lock management (test-and-set)
- Shared memory access (memory ordering)
"""

import asyncio
import asyncpg
import logging
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from .atomics_config import ColumnType, QueryPattern, TableSchema

logger = logging.getLogger(__name__)


@dataclass
class QueryResult:
    """Result of a single query execution."""
    query_pattern: str
    table_name: str
    implementation: str  # "traditional" or "stdatomic"
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
    implementation: str
    results: List[QueryResult] = field(default_factory=list)

    def add(self, result: QueryResult):
        self.results.append(result)


class AtomicsWorkloadRunner:
    """Generates and executes query workloads for atomics testing."""

    def __init__(
        self,
        pool: asyncpg.Pool,
        warmup_iterations: int = 3,
        measure_iterations: int = 10,
    ):
        self.pool = pool
        self.warmup_iterations = warmup_iterations
        self.measure_iterations = measure_iterations

    # ------------------------------------------------------------------
    # Query generators per pattern
    # ------------------------------------------------------------------

    def _full_scan_query(self, table_name: str, schema: TableSchema) -> str:
        """Full table scan - stresses spinlock acquisition and buffer management."""
        return f"SELECT * FROM {table_name}"

    def _column_projection_query(self, table_name: str, schema: TableSchema) -> str:
        """Column projection - tuple visibility checks with atomic reads."""
        cols = [c[0] for c in schema.columns if c[0] != "id"][:2]
        if not cols:
            cols = [schema.columns[0][0]]
        return f"SELECT {', '.join(cols)} FROM {table_name}"

    def _filtered_scan_query(self, table_name: str, schema: TableSchema) -> str:
        """Filtered scan - mixed index and sequential access patterns."""
        for col_name, col_type in schema.columns:
            if col_type == ColumnType.INT and col_name != "id":
                return f"SELECT * FROM {table_name} WHERE {col_name} > 0"
            if col_type == ColumnType.BOOLEAN:
                return f"SELECT * FROM {table_name} WHERE {col_name} = TRUE"
        return f"SELECT * FROM {table_name} WHERE id > 100 AND id <= 10000"

    def _aggregation_query(self, table_name: str, schema: TableSchema) -> str:
        """Aggregation - heavy shared buffer access with memory ordering stress."""
        agg_exprs = []
        for col_name, col_type in schema.columns:
            if col_type in (
                ColumnType.INT,
                ColumnType.BIGINT,
                ColumnType.FLOAT,
                ColumnType.NUMERIC,
            ):
                agg_exprs.append(f"SUM({col_name})")
                agg_exprs.append(f"AVG({col_name})")
                if len(agg_exprs) >= 6:
                    break
        if not agg_exprs:
            agg_exprs = ["COUNT(*)"]
        return f"SELECT COUNT(*), {', '.join(agg_exprs)} FROM {table_name}"

    def _group_by_query(self, table_name: str, schema: TableSchema) -> str:
        """Group-by with hash aggregation - memory ordering and buffer management."""
        group_col = None
        agg_col = None
        for col_name, col_type in schema.columns:
            if col_name == "id":
                continue
            if col_type in (ColumnType.INT, ColumnType.BOOLEAN) and group_col is None:
                group_col = col_name
            if (
                col_type
                in (ColumnType.FLOAT, ColumnType.NUMERIC, ColumnType.INT, ColumnType.BIGINT)
                and agg_col is None
            ):
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
        """Index scan - buffer pin/unpin contention."""
        return f"SELECT * FROM {table_name} WHERE id BETWEEN 100 AND 200"

    def _concurrent_insert_query(self, table_name: str, schema: TableSchema) -> str:
        """Concurrent inserts - high contention lock management."""
        # Generate a simple insert
        col_names = schema.column_names
        values = []
        for col_name, col_type in schema.columns:
            if col_name == "id":
                values.append("nextval('seq_concurrent_insert')")
            elif col_type == ColumnType.INT:
                values.append("42")
            elif col_type == ColumnType.BIGINT:
                values.append("12345")
            elif col_type == ColumnType.TEXT:
                values.append("'test'")
            elif col_type == ColumnType.BOOLEAN:
                values.append("TRUE")
            elif col_type == ColumnType.UUID:
                values.append("gen_random_uuid()")
            elif col_type == ColumnType.TIMESTAMP:
                values.append("NOW()")
            elif col_type == ColumnType.FLOAT:
                values.append("3.14")
            elif col_type == ColumnType.NUMERIC:
                values.append("99.99")
            elif col_type == ColumnType.JSONB:
                values.append("'{}'::jsonb")
            else:
                values.append("NULL")

        return f"INSERT INTO {table_name} ({', '.join(col_names)}) VALUES ({', '.join(values)})"

    def _concurrent_update_query(self, table_name: str, schema: TableSchema) -> str:
        """Concurrent updates - lock management and visibility checks."""
        update_col = None
        for col_name, col_type in schema.columns:
            if col_name != "id" and col_type in (ColumnType.INT, ColumnType.BIGINT):
                update_col = col_name
                break

        if update_col is None:
            update_col = "id"

        return f"UPDATE {table_name} SET {update_col} = {update_col} + 1 WHERE id <= 100"

    def _mixed_workload_query(self, table_name: str, schema: TableSchema) -> str:
        """Mixed read/write workload - realistic contention patterns."""
        return f"""
        WITH updated AS (
            UPDATE {table_name} SET id = id WHERE id <= 10 RETURNING *
        )
        SELECT COUNT(*) FROM {table_name} WHERE id > 10
        """

    def _get_query(
        self, pattern: QueryPattern, table_name: str, schema: TableSchema
    ) -> str:
        """Get query for a given pattern."""
        generators = {
            QueryPattern.FULL_SCAN: self._full_scan_query,
            QueryPattern.COLUMN_PROJECTION: self._column_projection_query,
            QueryPattern.FILTERED_SCAN: self._filtered_scan_query,
            QueryPattern.AGGREGATION: self._aggregation_query,
            QueryPattern.GROUP_BY: self._group_by_query,
            QueryPattern.INDEX_SCAN: self._index_scan_query,
            QueryPattern.CONCURRENT_INSERT: self._concurrent_insert_query,
            QueryPattern.CONCURRENT_UPDATE: self._concurrent_update_query,
            QueryPattern.MIXED_WORKLOAD: self._mixed_workload_query,
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
        implementation: str,
        collect_explain: bool = False,
    ) -> QueryResult:
        """Run a single query, returning timing and optional EXPLAIN data."""
        # Warmup
        async with self.pool.acquire() as conn:
            for _ in range(self.warmup_iterations):
                try:
                    await conn.fetch(query)
                except Exception as e:
                    logger.warning("Warmup query failed: %s", e)

        # Measure
        timings = []
        for _ in range(self.measure_iterations):
            start = time.perf_counter()
            async with self.pool.acquire() as conn:
                try:
                    rows = await conn.fetch(query)
                    elapsed = time.perf_counter() - start
                    timings.append(elapsed)
                except Exception as e:
                    logger.error("Measurement query failed: %s", e)
                    elapsed = 0.0
                    timings.append(elapsed)

        # Use median timing
        timings.sort()
        median_time = timings[len(timings) // 2]

        # Optional EXPLAIN
        explain_plan = None
        if collect_explain:
            try:
                async with self.pool.acquire() as conn:
                    explain_result = await conn.fetch(
                        f"EXPLAIN (FORMAT JSON, ANALYZE, BUFFERS) {query}"
                    )
                    if explain_result:
                        explain_plan = explain_result[0][0]
            except Exception as e:
                logger.warning("EXPLAIN failed: %s", e)

        return QueryResult(
            query_pattern=pattern.value,
            table_name=table_name,
            implementation=implementation,
            query_sql=query,
            elapsed_seconds=median_time,
            row_count=len(rows) if rows else 0,
            explain_plan=explain_plan,
        )

    async def run_workload(
        self,
        schema: TableSchema,
        table_name: str,
        row_count: int,
        distribution: str,
        patterns: List[QueryPattern],
    ) -> WorkloadResult:
        """Run complete workload and return results."""
        result = WorkloadResult(
            schema_name=schema.name,
            row_count=row_count,
            distribution=distribution,
            implementation="",  # Set by caller
        )

        for pattern in patterns:
            try:
                query = self._get_query(pattern, table_name, schema)
                query_result = await self._run_single(
                    query=query,
                    pattern=pattern,
                    table_name=table_name,
                    implementation=result.implementation,
                    collect_explain=False,  # Too expensive for full matrix
                )
                result.add(query_result)

                logger.info(
                    "  %s: %.2fms",
                    pattern.value,
                    query_result.elapsed_seconds * 1000,
                )
            except Exception as e:
                logger.error("Pattern %s failed: %s", pattern.value, e, exc_info=True)

        return result
