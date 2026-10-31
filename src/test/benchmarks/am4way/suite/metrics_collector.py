"""
Metrics collector: extracts pg_stat_statements data and compression
statistics from pg_statistic and Noxu internal catalogs.
"""

import logging
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from .database import DatabaseManager

logger = logging.getLogger(__name__)


@dataclass
class StorageMetrics:
    """Storage size and compression metrics for a single table."""
    table_name: str
    storage_method: str
    table_size_bytes: int = 0
    index_size_bytes: int = 0
    total_size_bytes: int = 0
    row_count: int = 0
    dead_tuples: int = 0
    # Compression stats (Noxu-specific)
    compression_ratio: float = 1.0
    pages_compressed: int = 0
    pages_total: int = 0


@dataclass
class QueryMetrics:
    """Aggregated query-level metrics from pg_stat_statements."""
    query_pattern: str
    calls: int = 0
    total_time_ms: float = 0.0
    mean_time_ms: float = 0.0
    min_time_ms: float = 0.0
    max_time_ms: float = 0.0
    stddev_time_ms: float = 0.0
    rows: int = 0
    shared_blks_hit: int = 0
    shared_blks_read: int = 0
    shared_blks_written: int = 0
    temp_blks_read: int = 0
    temp_blks_written: int = 0

    @property
    def cache_hit_ratio(self) -> float:
        total = self.shared_blks_hit + self.shared_blks_read
        if total == 0:
            return 0.0
        return self.shared_blks_hit / total


@dataclass
class BenchmarkMetrics:
    """Complete metrics collection for a benchmark run."""
    schema_name: str
    row_count: int
    distribution: str
    heap_storage: Optional[StorageMetrics] = None
    noxu_storage: Optional[StorageMetrics] = None
    query_metrics: List[QueryMetrics] = field(default_factory=list)
    pg_stat_entries: List[Dict[str, Any]] = field(default_factory=list)
    compression_stats: Dict[str, Any] = field(default_factory=dict)

    @property
    def compression_ratio(self) -> float:
        """Overall storage compression ratio (heap_size / noxu_size)."""
        if self.heap_storage and self.noxu_storage:
            if self.noxu_storage.total_size_bytes > 0:
                return (
                    self.heap_storage.total_size_bytes
                    / self.noxu_storage.total_size_bytes
                )
        return 1.0


class MetricsCollector:
    """Collects storage, query, and compression metrics."""

    def __init__(self, db: DatabaseManager):
        self.db = db

    async def collect_storage_metrics(
        self, table_name: str, storage_method: str
    ) -> StorageMetrics:
        """Collect storage size metrics for a table."""
        metrics = StorageMetrics(
            table_name=table_name,
            storage_method=storage_method,
        )

        sizes = await self.db.get_table_size(table_name)
        metrics.table_size_bytes = sizes["table_size"]
        metrics.index_size_bytes = sizes["index_size"]
        metrics.total_size_bytes = sizes["total_size"]

        # Row count from pg_stat_user_tables (fast, approximate)
        row = await self.db.fetchrow(
            """
            SELECT n_live_tup, n_dead_tup
            FROM pg_stat_user_tables
            WHERE relname = $1
            """,
            table_name,
        )
        if row:
            metrics.row_count = row["n_live_tup"] or 0
            metrics.dead_tuples = row["n_dead_tup"] or 0

        # Page counts from pg_class
        row = await self.db.fetchrow(
            "SELECT relpages, reltuples FROM pg_class WHERE relname = $1",
            table_name,
        )
        if row:
            metrics.pages_total = row["relpages"] or 0

        logger.info(
            "Storage metrics for %s: table=%d bytes, index=%d bytes, total=%d bytes",
            table_name,
            metrics.table_size_bytes,
            metrics.index_size_bytes,
            metrics.total_size_bytes,
        )
        return metrics

    async def collect_compression_stats(
        self, table_name: str
    ) -> Dict[str, Any]:
        """Collect compression statistics from pg_statistic for a table.

        This extracts per-column statistics that indicate compression
        effectiveness: null fraction, distinct values, average width,
        and most common values.
        """
        stats = {}
        try:
            rows = await self.db.fetch(
                """
                SELECT
                    a.attname AS column_name,
                    a.atttypid::regtype AS column_type,
                    s.stanullfrac AS null_fraction,
                    s.stadistinct AS n_distinct,
                    s.stawidth AS avg_width,
                    CASE
                        WHEN s.stakind1 = 1 THEN s.stanumbers1
                        ELSE NULL
                    END AS most_common_freqs
                FROM pg_statistic s
                JOIN pg_attribute a ON a.attrelid = s.starelid
                    AND a.attnum = s.staattnum
                WHERE s.starelid = $1::regclass
                ORDER BY a.attnum
                """,
                table_name,
            )
            for row in rows:
                col_stats = {
                    "column_type": str(row["column_type"]),
                    "null_fraction": float(row["null_fraction"] or 0),
                    "n_distinct": float(row["n_distinct"] or 0),
                    "avg_width": int(row["avg_width"] or 0),
                }
                freqs = row["most_common_freqs"]
                if freqs:
                    col_stats["top_freq_sum"] = sum(float(f) for f in freqs[:5])
                stats[row["column_name"]] = col_stats
        except Exception as e:
            logger.warning(
                "Could not collect compression stats for %s: %s", table_name, e
            )
        return stats

    async def collect_noxu_internals(
        self, table_name: str
    ) -> Dict[str, Any]:
        """Collect Noxu-specific internal statistics if available.

        Queries noxu_inspect functions for page-level compression data.
        """
        internals = {}
        try:
            # Check if inspect function exists
            exists = await self.db.fetchval(
                """
                SELECT EXISTS(
                    SELECT 1 FROM pg_proc WHERE proname = 'noxu_inspect'
                )
                """
            )
            if not exists:
                logger.debug("noxu_inspect function not found; skipping internals")
                return internals

            rows = await self.db.fetch(
                f"SELECT * FROM noxu_inspect('{table_name}'::regclass)"
            )
            if rows:
                internals["pages"] = [dict(r) for r in rows]
                total_pages = len(rows)
                compressed_pages = sum(
                    1 for r in rows if r.get("compressed", False)
                )
                internals["total_pages"] = total_pages
                internals["compressed_pages"] = compressed_pages
                if total_pages > 0:
                    internals["compression_pct"] = (
                        compressed_pages / total_pages * 100
                    )
        except Exception as e:
            logger.debug("Could not collect Noxu internals for %s: %s", table_name, e)
        return internals

    async def collect_all(
        self,
        heap_table: str,
        noxu_table: str,
        schema_name: str,
        row_count: int,
        distribution: str,
    ) -> BenchmarkMetrics:
        """Collect all metrics for a benchmark pair."""
        metrics = BenchmarkMetrics(
            schema_name=schema_name,
            row_count=row_count,
            distribution=distribution,
        )

        metrics.heap_storage = await self.collect_storage_metrics(heap_table, "heap")
        metrics.noxu_storage = await self.collect_storage_metrics(
            noxu_table, "noxu"
        )

        # Compression stats from pg_statistic for both
        heap_comp = await self.collect_compression_stats(heap_table)
        noxu_comp = await self.collect_compression_stats(noxu_table)
        metrics.compression_stats = {
            "heap": heap_comp,
            "noxu": noxu_comp,
        }

        # Noxu internal page stats
        noxu_internals = await self.collect_noxu_internals(noxu_table)
        if noxu_internals:
            metrics.compression_stats["noxu_internals"] = noxu_internals

        # pg_stat_statements
        metrics.pg_stat_entries = await self.db.get_pg_stat_statements()

        logger.info(
            "Compression ratio for %s/%s: %.2fx",
            heap_table,
            noxu_table,
            metrics.compression_ratio,
        )
        return metrics
