"""
Benchmark configuration: connection pooling, test parameters, and matrix definitions.
"""

import os
from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional


class TableWidth(Enum):
    NARROW = "narrow"      # 3-5 columns
    MEDIUM = "medium"      # 10-30 columns
    WIDE = "wide"          # 50-120 columns


class DataDistribution(Enum):
    RANDOM = "random"
    CLUSTERED = "clustered"
    LOW_CARDINALITY = "low_cardinality"
    HIGH_NULL = "high_null"


class QueryPattern(Enum):
    FULL_SCAN = "full_scan"
    COLUMN_PROJECTION = "column_projection"
    FILTERED_SCAN = "filtered_scan"
    AGGREGATION = "aggregation"
    GROUP_BY = "group_by"
    INDEX_SCAN = "index_scan"


class ColumnType(Enum):
    INT = "integer"
    BIGINT = "bigint"
    TEXT = "text"
    BOOLEAN = "boolean"
    UUID = "uuid"
    TIMESTAMP = "timestamp"
    FLOAT = "double precision"
    NUMERIC = "numeric(12,2)"
    JSONB = "jsonb"


ROW_COUNTS = [1_000, 10_000, 100_000, 1_000_000, 10_000_000, 100_000_000]

# Smaller default for quick runs
DEFAULT_ROW_COUNTS = [1_000, 10_000, 100_000]


@dataclass
class ConnectionConfig:
    host: str = "localhost"
    port: int = 5432
    database: str = "benchmark_db"
    user: str = ""
    password: str = ""
    min_pool_size: int = 2
    max_pool_size: int = 10
    statement_cache_size: int = 100

    def __post_init__(self):
        self.host = os.environ.get("PGHOST", self.host)
        self.port = int(os.environ.get("PGPORT", str(self.port)))
        self.database = os.environ.get("PGDATABASE", self.database)
        self.user = os.environ.get("PGUSER", self.user) or os.environ.get("USER", "")
        self.password = os.environ.get("PGPASSWORD", self.password)

    @property
    def dsn(self) -> str:
        parts = [f"host={self.host}", f"port={self.port}", f"dbname={self.database}"]
        if self.user:
            parts.append(f"user={self.user}")
        if self.password:
            parts.append(f"password={self.password}")
        return " ".join(parts)


@dataclass
class TableSchema:
    """Defines a table schema for benchmarking."""
    name: str
    width: TableWidth
    columns: List[tuple]  # (col_name, ColumnType)
    index_columns: List[str] = field(default_factory=list)

    @property
    def column_names(self) -> List[str]:
        return [c[0] for c in self.columns]

    @property
    def column_types(self) -> List[ColumnType]:
        return [c[1] for c in self.columns]


# Pre-defined table schemas for the test matrix
NARROW_SCHEMA = TableSchema(
    name="bench_narrow",
    width=TableWidth.NARROW,
    columns=[
        ("id", ColumnType.BIGINT),
        ("val_int", ColumnType.INT),
        ("val_text", ColumnType.TEXT),
        ("flag", ColumnType.BOOLEAN),
    ],
    index_columns=["id"],
)

MEDIUM_SCHEMA = TableSchema(
    name="bench_medium",
    width=TableWidth.MEDIUM,
    columns=[
        ("id", ColumnType.BIGINT),
        ("category", ColumnType.INT),
        ("amount", ColumnType.NUMERIC),
        ("description", ColumnType.TEXT),
        ("is_active", ColumnType.BOOLEAN),
        ("created_at", ColumnType.TIMESTAMP),
        ("ref_uuid", ColumnType.UUID),
        ("score", ColumnType.FLOAT),
        ("status_code", ColumnType.INT),
        ("notes", ColumnType.TEXT),
        ("metadata", ColumnType.JSONB),
    ],
    index_columns=["id", "category"],
)

def _build_wide_columns():
    """Build a wide schema with 55 columns covering all data types."""
    cols = [("id", ColumnType.BIGINT)]
    # 8 INT columns
    for i in range(1, 9):
        cols.append((f"col_int_{i}", ColumnType.INT))
    # 5 BIGINT columns
    for i in range(1, 6):
        cols.append((f"col_bigint_{i}", ColumnType.BIGINT))
    # 8 TEXT columns
    for i in range(1, 9):
        cols.append((f"col_text_{i}", ColumnType.TEXT))
    # 6 BOOLEAN columns
    for i in range(1, 7):
        cols.append((f"col_bool_{i}", ColumnType.BOOLEAN))
    # 5 FLOAT columns
    for i in range(1, 6):
        cols.append((f"col_float_{i}", ColumnType.FLOAT))
    # 5 NUMERIC columns
    for i in range(1, 6):
        cols.append((f"col_numeric_{i}", ColumnType.NUMERIC))
    # 5 UUID columns
    for i in range(1, 6):
        cols.append((f"col_uuid_{i}", ColumnType.UUID))
    # 5 TIMESTAMP columns
    for i in range(1, 6):
        cols.append((f"col_ts_{i}", ColumnType.TIMESTAMP))
    # 4 JSONB columns
    for i in range(1, 5):
        cols.append((f"col_jsonb_{i}", ColumnType.JSONB))
    # 3 more INT columns to reach 55
    for i in range(9, 12):
        cols.append((f"col_int_{i}", ColumnType.INT))
    return cols


WIDE_SCHEMA = TableSchema(
    name="bench_wide",
    width=TableWidth.WIDE,
    columns=_build_wide_columns(),
    index_columns=["id", "col_int_1", "col_text_1"],
)

ALL_SCHEMAS = [NARROW_SCHEMA, MEDIUM_SCHEMA, WIDE_SCHEMA]


@dataclass
class BenchmarkConfig:
    """Top-level benchmark configuration."""
    connection: ConnectionConfig = field(default_factory=ConnectionConfig)
    schemas: List[TableSchema] = field(default_factory=lambda: list(ALL_SCHEMAS))
    row_counts: List[int] = field(default_factory=lambda: list(DEFAULT_ROW_COUNTS))
    distributions: List[DataDistribution] = field(
        default_factory=lambda: [
            DataDistribution.RANDOM,
            DataDistribution.CLUSTERED,
            DataDistribution.LOW_CARDINALITY,
            DataDistribution.HIGH_NULL,
        ]
    )
    query_patterns: List[QueryPattern] = field(
        default_factory=lambda: list(QueryPattern)
    )
    warmup_iterations: int = 2
    measure_iterations: int = 5
    seed: int = 42
    output_dir: str = "benchmark_results"
    enable_pg_stat_statements: bool = True
    enable_compression_stats: bool = True
    verbose: bool = False
    # Run the full matrix or a reduced subset
    full_matrix: bool = False

    def get_row_counts(self) -> List[int]:
        if self.full_matrix:
            return ROW_COUNTS
        return self.row_counts
