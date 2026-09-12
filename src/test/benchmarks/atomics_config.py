"""
Benchmark configuration for comparing traditional vs stdatomic.h implementations.

This module defines the test matrix for evaluating atomics performance impact
across different workload patterns, table schemas, and data distributions.
"""

import os
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import List, Optional


class TableWidth(Enum):
    """Table width categories for testing atomic operation overhead."""
    NARROW = "narrow"      # 3-5 columns - minimal overhead
    MEDIUM = "medium"      # 10-30 columns - moderate overhead
    WIDE = "wide"          # 50-120 columns - heavy overhead


class DataDistribution(Enum):
    """Data distribution patterns affecting atomic contention."""
    RANDOM = "random"
    CLUSTERED = "clustered"
    LOW_CARDINALITY = "low_cardinality"
    HIGH_NULL = "high_null"


class QueryPattern(Enum):
    """Query patterns that stress different atomic operations."""
    FULL_SCAN = "full_scan"              # Sequential scan - spinlock acquisition
    COLUMN_PROJECTION = "column_projection"  # Selective scan - tuple visibility checks
    FILTERED_SCAN = "filtered_scan"      # Index + seq scan - mixed contention
    AGGREGATION = "aggregation"          # Heavy computation - shared buffer access
    GROUP_BY = "group_by"                # Hash aggregation - memory ordering stress
    INDEX_SCAN = "index_scan"            # Index-only - buffer pin/unpin contention
    CONCURRENT_INSERT = "concurrent_insert"  # High contention writes
    CONCURRENT_UPDATE = "concurrent_update"  # Lock management stress
    MIXED_WORKLOAD = "mixed_workload"    # Read/write mix - realistic contention


class ColumnType(Enum):
    """Column types for schema generation."""
    INT = "integer"
    BIGINT = "bigint"
    TEXT = "text"
    BOOLEAN = "boolean"
    UUID = "uuid"
    TIMESTAMP = "timestamp"
    FLOAT = "double precision"
    NUMERIC = "numeric(12,2)"
    JSONB = "jsonb"


# Row counts for testing atomic overhead at different scales
ROW_COUNTS = [1_000, 10_000, 100_000, 1_000_000, 10_000_000]

# Default subset for quick runs
DEFAULT_ROW_COUNTS = [10_000, 100_000, 1_000_000]


@dataclass
class PostgresInstance:
    """Configuration for a PostgreSQL instance (traditional or stdatomic build)."""
    name: str                    # "traditional" or "stdatomic"
    build_dir: Path             # Path to build or install directory
    host: str = "localhost"
    port: int = 5432
    data_dir: Optional[Path] = None
    user: str = ""
    password: str = ""

    def _is_install_dir(self) -> bool:
        """Check if build_dir is an install directory (has bin/ subdirectory)."""
        return (self.build_dir / "bin").exists()

    @property
    def postgres_bin(self) -> Path:
        """Path to postgres binary."""
        if self._is_install_dir():
            return self.build_dir / "bin" / "postgres"
        return self.build_dir / "src" / "backend" / "postgres"

    @property
    def initdb_bin(self) -> Path:
        """Path to initdb binary."""
        if self._is_install_dir():
            return self.build_dir / "bin" / "initdb"
        return self.build_dir / "src" / "bin" / "initdb" / "initdb"

    @property
    def pg_ctl_bin(self) -> Path:
        """Path to pg_ctl binary."""
        if self._is_install_dir():
            return self.build_dir / "bin" / "pg_ctl"
        return self.build_dir / "src" / "bin" / "pg_ctl" / "pg_ctl"

    def get_dsn(self, database: str = "postgres") -> str:
        """Get connection DSN."""
        parts = [
            f"host={self.host}",
            f"port={self.port}",
            f"dbname={database}",
        ]
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
    name="atomics_narrow",
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
    name="atomics_medium",
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
    name="atomics_wide",
    width=TableWidth.WIDE,
    columns=_build_wide_columns(),
    index_columns=["id", "col_int_1", "col_text_1"],
)

ALL_SCHEMAS = [NARROW_SCHEMA, MEDIUM_SCHEMA, WIDE_SCHEMA]


@dataclass
class AtomicsBenchmarkConfig:
    """Configuration for atomics performance comparison."""
    # PostgreSQL instances to compare
    traditional_instance: Optional[PostgresInstance] = None
    stdatomic_instance: Optional[PostgresInstance] = None

    # Test matrix
    schemas: List[TableSchema] = field(default_factory=lambda: list(ALL_SCHEMAS))
    row_counts: List[int] = field(default_factory=lambda: list(DEFAULT_ROW_COUNTS))
    distributions: List[DataDistribution] = field(
        default_factory=lambda: [
            DataDistribution.RANDOM,
            DataDistribution.CLUSTERED,
            DataDistribution.LOW_CARDINALITY,
        ]
    )
    query_patterns: List[QueryPattern] = field(
        default_factory=lambda: [
            QueryPattern.FULL_SCAN,
            QueryPattern.COLUMN_PROJECTION,
            QueryPattern.FILTERED_SCAN,
            QueryPattern.AGGREGATION,
            QueryPattern.GROUP_BY,
            QueryPattern.INDEX_SCAN,
        ]
    )

    # Execution parameters
    warmup_iterations: int = 3
    measure_iterations: int = 10
    concurrent_clients: int = 8  # For concurrent workload tests
    seed: int = 42

    # Output
    output_dir: str = "atomics_benchmark_results"
    verbose: bool = False

    # Run the full matrix or a reduced subset
    full_matrix: bool = False

    def __post_init__(self):
        """Initialize default instances if not provided."""
        if self.traditional_instance is None:
            build_dir = Path.cwd() / "build-traditional"
            if not build_dir.exists():
                build_dir = Path.cwd() / "build"
            self.traditional_instance = PostgresInstance(
                name="traditional",
                build_dir=build_dir,
                port=5433,
                data_dir=Path("/tmp/pgdata-traditional"),
            )

        if self.stdatomic_instance is None:
            build_dir = Path.cwd() / "build-stdatomic"
            self.stdatomic_instance = PostgresInstance(
                name="stdatomic",
                build_dir=build_dir,
                port=5434,
                data_dir=Path("/tmp/pgdata-stdatomic"),
            )

    def get_row_counts(self) -> List[int]:
        if self.full_matrix:
            return ROW_COUNTS
        return self.row_counts
