"""
Reproducible seeded random data generation for benchmark tables.

Generates SQL INSERT statements or COPY-compatible data for various
column types and data distributions.
"""

import hashlib
import logging
import random
import uuid
from datetime import datetime, timedelta
from typing import Any, List, Optional

from .atomics_config import ColumnType, DataDistribution, TableSchema

logger = logging.getLogger(__name__)

# Low-cardinality value pools
LOW_CARD_TEXT = [
    "active", "inactive", "pending", "completed", "cancelled",
    "processing", "shipped", "returned", "refunded", "on_hold",
]
LOW_CARD_INT_RANGE = 20
LOW_CARD_STATUS_CODES = [100, 200, 201, 301, 400, 403, 404, 500, 502, 503]

# Clustered parameters
CLUSTER_CENTERS = 5
CLUSTER_SPREAD = 100

# Base timestamp for reproducible timestamp generation
BASE_TS = datetime(2020, 1, 1)


class DataGenerator:
    """Generates reproducible test data for benchmark tables."""

    def __init__(self, seed: int = 42):
        self.seed = seed
        self._rng = random.Random(seed)

    def reset(self):
        """Reset the RNG to produce identical sequences."""
        self._rng = random.Random(self.seed)

    # ------------------------------------------------------------------
    # Value generators per column type and distribution
    # ------------------------------------------------------------------

    def _gen_int(self, dist: DataDistribution, row_idx: int) -> int:
        if dist == DataDistribution.RANDOM:
            return self._rng.randint(-2_147_483_648, 2_147_483_647)
        elif dist == DataDistribution.CLUSTERED:
            center = (row_idx % CLUSTER_CENTERS) * 1_000_000
            return center + self._rng.randint(-CLUSTER_SPREAD, CLUSTER_SPREAD)
        else:  # LOW_CARDINALITY
            return self._rng.choice(LOW_CARD_STATUS_CODES)

    def _gen_bigint(self, dist: DataDistribution, row_idx: int) -> int:
        if dist == DataDistribution.RANDOM:
            return self._rng.randint(0, 2**62)
        elif dist == DataDistribution.CLUSTERED:
            center = (row_idx % CLUSTER_CENTERS) * 10_000_000_000
            return center + self._rng.randint(-1000, 1000)
        else:
            return self._rng.randint(1, LOW_CARD_INT_RANGE)

    def _gen_text(self, dist: DataDistribution, row_idx: int) -> str:
        if dist == DataDistribution.RANDOM:
            # MD5-like random string
            h = hashlib.md5(f"{self.seed}-{row_idx}-{self._rng.random()}".encode())
            return h.hexdigest()
        elif dist == DataDistribution.CLUSTERED:
            group = row_idx % CLUSTER_CENTERS
            suffix = self._rng.randint(0, CLUSTER_SPREAD)
            return f"group_{group}_item_{suffix}"
        else:
            return self._rng.choice(LOW_CARD_TEXT)

    def _gen_boolean(self, dist: DataDistribution, row_idx: int) -> bool:
        if dist == DataDistribution.RANDOM:
            return self._rng.random() < 0.5
        elif dist == DataDistribution.CLUSTERED:
            # Runs of True/False
            return (row_idx // 100) % 2 == 0
        else:
            # Heavily skewed: 95% True
            return self._rng.random() < 0.95

    def _gen_uuid(self, dist: DataDistribution, row_idx: int) -> str:
        if dist == DataDistribution.LOW_CARDINALITY:
            # Only 10 distinct UUIDs
            idx = row_idx % 10
            return str(uuid.UUID(int=idx + 1))
        # For RANDOM and CLUSTERED, use seeded generation
        bits = self._rng.getrandbits(128)
        return str(uuid.UUID(int=bits, version=4))

    def _gen_timestamp(self, dist: DataDistribution, row_idx: int) -> str:
        if dist == DataDistribution.RANDOM:
            days = self._rng.randint(0, 1825)  # ~5 years
            secs = self._rng.randint(0, 86400)
            ts = BASE_TS + timedelta(days=days, seconds=secs)
        elif dist == DataDistribution.CLUSTERED:
            # Clustered around specific dates
            center_day = (row_idx % CLUSTER_CENTERS) * 365
            offset = self._rng.randint(-30, 30)
            ts = BASE_TS + timedelta(days=center_day + offset)
        else:
            # Low cardinality: 10 distinct dates
            day_idx = row_idx % 10
            ts = BASE_TS + timedelta(days=day_idx * 100)
        return ts.strftime("%Y-%m-%d %H:%M:%S")

    def _gen_float(self, dist: DataDistribution, row_idx: int) -> float:
        if dist == DataDistribution.RANDOM:
            return self._rng.uniform(-1e6, 1e6)
        elif dist == DataDistribution.CLUSTERED:
            center = (row_idx % CLUSTER_CENTERS) * 1000.0
            return center + self._rng.gauss(0, 10)
        else:
            return self._rng.choice([0.0, 1.0, 10.0, 100.0, 1000.0])

    def _gen_numeric(self, dist: DataDistribution, row_idx: int) -> str:
        val = self._gen_float(dist, row_idx)
        return f"{val:.2f}"

    def _gen_jsonb(self, dist: DataDistribution, row_idx: int) -> str:
        import json
        if dist == DataDistribution.RANDOM:
            obj = {
                "key": self._rng.randint(1, 100000),
                "label": hashlib.md5(f"{self.seed}-json-{row_idx}".encode()).hexdigest()[:8],
                "value": round(self._rng.uniform(0, 1000), 2),
                "active": self._rng.random() < 0.5,
            }
        elif dist == DataDistribution.CLUSTERED:
            group = row_idx % CLUSTER_CENTERS
            obj = {
                "group": group,
                "label": f"cluster_{group}",
                "value": group * 100 + self._rng.randint(0, CLUSTER_SPREAD),
            }
        elif dist == DataDistribution.HIGH_NULL:
            # HIGH_NULL: return None most of the time (handled in _gen_value)
            obj = {"id": row_idx % 10, "status": self._rng.choice(LOW_CARD_TEXT)}
        else:  # LOW_CARDINALITY
            obj = {"id": row_idx % 10, "status": self._rng.choice(LOW_CARD_TEXT)}
        return json.dumps(obj)

    def _gen_value(
        self, col_type: ColumnType, dist: DataDistribution, row_idx: int
    ) -> Any:
        # HIGH_NULL distribution: ~80% of non-id values are NULL
        if dist == DataDistribution.HIGH_NULL and col_type != ColumnType.BIGINT:
            if self._rng.random() < 0.80:
                return None

        generators = {
            ColumnType.INT: self._gen_int,
            ColumnType.BIGINT: self._gen_bigint,
            ColumnType.TEXT: self._gen_text,
            ColumnType.BOOLEAN: self._gen_boolean,
            ColumnType.UUID: self._gen_uuid,
            ColumnType.TIMESTAMP: self._gen_timestamp,
            ColumnType.FLOAT: self._gen_float,
            ColumnType.NUMERIC: self._gen_numeric,
            ColumnType.JSONB: self._gen_jsonb,
        }
        gen = generators.get(col_type)
        if gen is None:
            raise ValueError(f"Unsupported column type: {col_type}")
        return gen(dist, row_idx)

    # ------------------------------------------------------------------
    # SQL generation helpers
    # ------------------------------------------------------------------

    def generate_insert_sql(
        self,
        schema: TableSchema,
        row_count: int,
        dist: DataDistribution,
        table_suffix: str = "",
        batch_size: int = 1000,
    ) -> List[str]:
        """Generate INSERT statements in batches for the given schema.

        Returns a list of SQL strings, each inserting up to batch_size rows.
        The ``id`` column is always set to the sequential row index.
        """
        self.reset()
        col_defs = ", ".join(schema.column_names)
        statements = []

        for batch_start in range(0, row_count, batch_size):
            batch_end = min(batch_start + batch_size, row_count)
            rows_sql = []
            for i in range(batch_start, batch_end):
                vals = []
                for col_name, col_type in schema.columns:
                    if col_name == "id":
                        vals.append(str(i + 1))
                    else:
                        v = self._gen_value(col_type, dist, i)
                        vals.append(self._sql_literal(v, col_type))
                rows_sql.append(f"({', '.join(vals)})")

            table_name = f"{schema.name}{table_suffix}"
            stmt = f"INSERT INTO {table_name} ({col_defs}) VALUES\n"
            stmt += ",\n".join(rows_sql)
            statements.append(stmt)

        return statements

    def generate_copy_data(
        self,
        schema: TableSchema,
        row_count: int,
        dist: DataDistribution,
    ) -> str:
        """Generate tab-separated COPY data for the given schema.

        Returns a single string suitable for COPY ... FROM STDIN.
        """
        self.reset()
        lines = []
        for i in range(row_count):
            vals = []
            for col_name, col_type in schema.columns:
                if col_name == "id":
                    vals.append(str(i + 1))
                else:
                    v = self._gen_value(col_type, dist, i)
                    vals.append(self._copy_literal(v, col_type))
            lines.append("\t".join(vals))
        return "\n".join(lines)

    def generate_server_side_insert(
        self,
        schema: TableSchema,
        row_count: int,
        dist: DataDistribution,
        table_suffix: str = "",
    ) -> str:
        """Generate a single INSERT ... SELECT generate_series SQL statement.

        This is much faster for large datasets because it runs entirely
        server-side without sending row data over the wire.
        """
        table_name = f"{schema.name}{table_suffix}"
        col_exprs = []
        for col_name, col_type in schema.columns:
            if col_name == "id":
                col_exprs.append("g AS id")
            else:
                col_exprs.append(
                    f"{self._server_side_expr(col_name, col_type, dist, row_count)} AS {col_name}"
                )

        select_list = ",\n       ".join(col_exprs)
        return (
            f"INSERT INTO {table_name} ({', '.join(schema.column_names)})\n"
            f"SELECT {select_list}\n"
            f"FROM generate_series(1, {row_count}) AS g"
        )

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    @staticmethod
    def _sql_literal(value: Any, col_type: ColumnType) -> str:
        if value is None:
            return "NULL"
        if col_type in (ColumnType.TEXT, ColumnType.UUID, ColumnType.TIMESTAMP):
            escaped = str(value).replace("'", "''")
            return f"'{escaped}'"
        if col_type == ColumnType.JSONB:
            escaped = str(value).replace("'", "''")
            return f"'{escaped}'::jsonb"
        if col_type == ColumnType.BOOLEAN:
            return "TRUE" if value else "FALSE"
        if col_type == ColumnType.NUMERIC:
            return str(value)
        return str(value)

    @staticmethod
    def _copy_literal(value: Any, col_type: ColumnType) -> str:
        if value is None:
            return "\\N"
        if col_type == ColumnType.BOOLEAN:
            return "t" if value else "f"
        return str(value)

    def _server_side_expr(
        self,
        col_name: str,
        col_type: ColumnType,
        dist: DataDistribution,
        row_count: int,
    ) -> str:
        """Return a SQL expression that produces the desired distribution
        server-side using generate_series variable ``g``."""

        seed_val = self.seed

        # HIGH_NULL: wrap the underlying RANDOM expression so ~80% are NULL
        if dist == DataDistribution.HIGH_NULL and col_type != ColumnType.BIGINT:
            inner = self._server_side_expr(
                col_name, col_type, DataDistribution.RANDOM, row_count
            )
            return f"CASE WHEN abs(hashint4(g + {seed_val} + 99)) % 5 = 0 THEN {inner} ELSE NULL END"

        if col_type == ColumnType.INT:
            if dist == DataDistribution.RANDOM:
                return f"(hashint4(g + {seed_val}) % 2147483647)::integer"
            elif dist == DataDistribution.CLUSTERED:
                return f"((g % {CLUSTER_CENTERS}) * 1000000 + (hashint4(g + {seed_val}) % {CLUSTER_SPREAD}))::integer"
            else:
                codes = ",".join(str(c) for c in LOW_CARD_STATUS_CODES)
                return f"(ARRAY[{codes}])[1 + abs(hashint4(g + {seed_val})) % {len(LOW_CARD_STATUS_CODES)}]"

        if col_type == ColumnType.BIGINT:
            if dist == DataDistribution.RANDOM:
                return f"(hashint8(g::bigint + {seed_val}) & x'3FFFFFFFFFFFFFFF'::bigint)::bigint"
            elif dist == DataDistribution.CLUSTERED:
                return f"((g % {CLUSTER_CENTERS})::bigint * 10000000000 + (hashint4(g + {seed_val}) % 1000)::bigint)"
            else:
                return f"(1 + abs(hashint4(g + {seed_val})) % {LOW_CARD_INT_RANGE})::bigint"

        if col_type == ColumnType.TEXT:
            if dist == DataDistribution.RANDOM:
                return f"md5(g::text || '{seed_val}')"
            elif dist == DataDistribution.CLUSTERED:
                return f"'group_' || (g % {CLUSTER_CENTERS})::text || '_item_' || (abs(hashint4(g + {seed_val})) % {CLUSTER_SPREAD})::text"
            else:
                texts = ",".join(f"'{t}'" for t in LOW_CARD_TEXT)
                return f"(ARRAY[{texts}])[1 + abs(hashint4(g + {seed_val})) % {len(LOW_CARD_TEXT)}]"

        if col_type == ColumnType.BOOLEAN:
            if dist == DataDistribution.RANDOM:
                return f"(hashint4(g + {seed_val}) % 2 = 0)"
            elif dist == DataDistribution.CLUSTERED:
                return f"((g / 100) % 2 = 0)"
            else:
                return f"(abs(hashint4(g + {seed_val})) % 20 != 0)"

        if col_type == ColumnType.UUID:
            if dist == DataDistribution.LOW_CARDINALITY:
                return f"(lpad(((g % 10) + 1)::text, 32, '0'))::uuid"
            return f"md5(g::text || '{seed_val}' || random()::text)::uuid"

        if col_type == ColumnType.TIMESTAMP:
            if dist == DataDistribution.RANDOM:
                return f"'2020-01-01'::timestamp + (abs(hashint4(g + {seed_val})) % 157680000) * interval '1 second'"
            elif dist == DataDistribution.CLUSTERED:
                return f"'2020-01-01'::timestamp + ((g % {CLUSTER_CENTERS}) * 365 + (abs(hashint4(g + {seed_val})) % 60) - 30) * interval '1 day'"
            else:
                return f"'2020-01-01'::timestamp + ((g % 10) * 100) * interval '1 day'"

        if col_type == ColumnType.FLOAT:
            if dist == DataDistribution.RANDOM:
                return f"(hashint4(g + {seed_val})::double precision / 2147483647.0 * 2000000 - 1000000)"
            elif dist == DataDistribution.CLUSTERED:
                return f"((g % {CLUSTER_CENTERS}) * 1000.0 + (hashint4(g + {seed_val}) % 100)::double precision / 10.0)"
            else:
                return f"(ARRAY[0.0, 1.0, 10.0, 100.0, 1000.0])[1 + abs(hashint4(g + {seed_val})) % 5]"

        if col_type == ColumnType.NUMERIC:
            if dist == DataDistribution.RANDOM:
                return f"round((hashint4(g + {seed_val})::numeric / 2147483647.0 * 2000000 - 1000000), 2)"
            elif dist == DataDistribution.CLUSTERED:
                return f"round(((g % {CLUSTER_CENTERS}) * 1000.0 + (hashint4(g + {seed_val}) % 100)::numeric / 10.0), 2)"
            else:
                return f"(ARRAY[0.00, 1.00, 10.00, 100.00, 1000.00])[1 + abs(hashint4(g + {seed_val})) % 5]::numeric(12,2)"

        if col_type == ColumnType.JSONB:
            if dist == DataDistribution.RANDOM:
                return (
                    f"jsonb_build_object("
                    f"'key', abs(hashint4(g + {seed_val})) % 100000, "
                    f"'label', left(md5(g::text || '{seed_val}'), 8), "
                    f"'value', round((hashint4(g + {seed_val})::numeric / 2147483647.0 * 1000), 2), "
                    f"'active', (hashint4(g + {seed_val}) % 2 = 0))"
                )
            elif dist == DataDistribution.CLUSTERED:
                return (
                    f"jsonb_build_object("
                    f"'group', g % {CLUSTER_CENTERS}, "
                    f"'label', 'cluster_' || (g % {CLUSTER_CENTERS})::text, "
                    f"'value', (g % {CLUSTER_CENTERS}) * 100 + abs(hashint4(g + {seed_val})) % {CLUSTER_SPREAD})"
                )
            elif dist == DataDistribution.HIGH_NULL:
                return (
                    f"CASE WHEN abs(hashint4(g + {seed_val})) % 5 = 0 THEN "
                    f"jsonb_build_object('id', g % 10, 'status', "
                    f"(ARRAY[{','.join(repr(t) for t in LOW_CARD_TEXT)}])"
                    f"[1 + abs(hashint4(g + {seed_val} + 1)) % {len(LOW_CARD_TEXT)}]) "
                    f"ELSE NULL END"
                )
            else:  # LOW_CARDINALITY
                texts = ",".join(f"'{t}'" for t in LOW_CARD_TEXT)
                return (
                    f"jsonb_build_object('id', g % 10, 'status', "
                    f"(ARRAY[{texts}])[1 + abs(hashint4(g + {seed_val})) % {len(LOW_CARD_TEXT)}])"
                )

        raise ValueError(f"Unsupported column type for server-side generation: {col_type}")
