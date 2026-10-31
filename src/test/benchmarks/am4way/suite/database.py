"""
Database connection manager using asyncpg with connection pooling and
pg_stat_statements integration.
"""

import asyncio
import logging
import time
from contextlib import asynccontextmanager
from typing import Any, Dict, List, Optional, Tuple

try:
    import asyncpg
except ImportError:
    asyncpg = None

from .config import ConnectionConfig

logger = logging.getLogger(__name__)


class DatabaseManager:
    """Manages asyncpg connection pool and provides query execution helpers."""

    def __init__(self, config: ConnectionConfig):
        self.config = config
        self._pool: Optional[Any] = None
        self._use_asyncpg = asyncpg is not None

    async def initialize(self):
        """Create the connection pool."""
        if not self._use_asyncpg:
            logger.warning(
                "asyncpg not installed; falling back to synchronous psycopg2"
            )
            return

        self._pool = await asyncpg.create_pool(
            host=self.config.host,
            port=self.config.port,
            database=self.config.database,
            user=self.config.user or None,
            password=self.config.password or None,
            min_size=self.config.min_pool_size,
            max_size=self.config.max_pool_size,
            statement_cache_size=self.config.statement_cache_size,
        )
        logger.info(
            "Connection pool created: %s:%s/%s (pool %d-%d)",
            self.config.host,
            self.config.port,
            self.config.database,
            self.config.min_pool_size,
            self.config.max_pool_size,
        )

    async def close(self):
        """Close the connection pool."""
        if self._pool:
            await self._pool.close()
            self._pool = None
            logger.info("Connection pool closed")

    @asynccontextmanager
    async def acquire(self):
        """Acquire a connection from the pool."""
        if not self._use_asyncpg or not self._pool:
            raise RuntimeError("Database not initialized or asyncpg not available")
        async with self._pool.acquire() as conn:
            yield conn

    async def execute(self, query: str, *args, timeout: float = 300.0) -> str:
        """Execute a query and return the status string."""
        async with self.acquire() as conn:
            return await conn.execute(query, *args, timeout=timeout)

    async def fetch(self, query: str, *args, timeout: float = 300.0) -> List[Any]:
        """Execute a query and return all rows."""
        async with self.acquire() as conn:
            return await conn.fetch(query, *args, timeout=timeout)

    async def fetchrow(self, query: str, *args, timeout: float = 300.0) -> Optional[Any]:
        """Execute a query and return one row."""
        async with self.acquire() as conn:
            return await conn.fetchrow(query, *args, timeout=timeout)

    async def fetchval(self, query: str, *args, timeout: float = 300.0) -> Any:
        """Execute a query and return a scalar value."""
        async with self.acquire() as conn:
            return await conn.fetchval(query, *args, timeout=timeout)

    async def execute_timed(
        self, query: str, *args, timeout: float = 300.0
    ) -> Tuple[Any, float]:
        """Execute a query and return (result, elapsed_seconds)."""
        start = time.perf_counter()
        result = await self.execute(query, *args, timeout=timeout)
        elapsed = time.perf_counter() - start
        return result, elapsed

    async def fetch_timed(
        self, query: str, *args, timeout: float = 300.0
    ) -> Tuple[List[Any], float]:
        """Fetch rows and return (rows, elapsed_seconds)."""
        start = time.perf_counter()
        rows = await self.fetch(query, *args, timeout=timeout)
        elapsed = time.perf_counter() - start
        return rows, elapsed

    # ------------------------------------------------------------------
    # pg_stat_statements helpers
    # ------------------------------------------------------------------

    async def reset_pg_stat_statements(self):
        """Reset pg_stat_statements counters."""
        try:
            await self.execute("SELECT pg_stat_statements_reset()")
            logger.debug("pg_stat_statements reset")
        except Exception as e:
            logger.warning("Could not reset pg_stat_statements: %s", e)

    async def get_pg_stat_statements(
        self, query_pattern: Optional[str] = None
    ) -> List[Dict[str, Any]]:
        """Retrieve pg_stat_statements entries, optionally filtered."""
        try:
            base = """
                SELECT queryid, query, calls, total_exec_time, mean_exec_time,
                       min_exec_time, max_exec_time, stddev_exec_time,
                       rows, shared_blks_hit, shared_blks_read,
                       shared_blks_written, temp_blks_read, temp_blks_written
                FROM pg_stat_statements
                WHERE dbid = (SELECT oid FROM pg_database WHERE datname = current_database())
            """
            if query_pattern:
                base += " AND query ILIKE $1"
                rows = await self.fetch(base + " ORDER BY total_exec_time DESC", query_pattern)
            else:
                rows = await self.fetch(base + " ORDER BY total_exec_time DESC")
            return [dict(r) for r in rows]
        except Exception as e:
            logger.warning("Could not query pg_stat_statements: %s", e)
            return []

    # ------------------------------------------------------------------
    # EXPLAIN ANALYZE helper
    # ------------------------------------------------------------------

    async def explain_analyze(
        self, query: str, *args, buffers: bool = True
    ) -> Dict[str, Any]:
        """Run EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) and return the plan."""
        options = "ANALYZE, FORMAT JSON"
        if buffers:
            options += ", BUFFERS"
        explain_query = f"EXPLAIN ({options}) {query}"
        rows = await self.fetch(explain_query, *args)
        if rows:
            plan = rows[0][0]
            if isinstance(plan, list):
                return plan[0]
            return plan
        return {}

    # ------------------------------------------------------------------
    # Utility
    # ------------------------------------------------------------------

    async def table_exists(self, table_name: str) -> bool:
        val = await self.fetchval(
            "SELECT EXISTS(SELECT 1 FROM pg_class WHERE relname = $1)", table_name
        )
        return bool(val)

    async def drop_table(self, table_name: str):
        await self.execute(f"DROP TABLE IF EXISTS {table_name} CASCADE")

    async def get_table_size(self, table_name: str) -> Dict[str, int]:
        """Return table size, index size, and total size in bytes."""
        row = await self.fetchrow(
            """
            SELECT pg_relation_size($1) AS table_size,
                   pg_indexes_size($1) AS index_size,
                   pg_total_relation_size($1) AS total_size
            """,
            table_name,
        )
        if row:
            return dict(row)
        return {"table_size": 0, "index_size": 0, "total_size": 0}

    async def vacuum_analyze(self, table_name: str):
        """Run VACUUM ANALYZE on a table (requires autocommit)."""
        async with self.acquire() as conn:
            await conn.execute(f"VACUUM ANALYZE {table_name}")

    async def ensure_extension(self, ext_name: str) -> bool:
        """Try to create an extension if it doesn't exist. Return True on success."""
        try:
            await self.execute(f"CREATE EXTENSION IF NOT EXISTS {ext_name}")
            return True
        except Exception as e:
            logger.warning("Could not create extension %s: %s", ext_name, e)
            return False

    async def check_noxu_available(self) -> bool:
        """Check whether the noxu table AM is registered."""
        val = await self.fetchval(
            "SELECT EXISTS(SELECT 1 FROM pg_am WHERE amname = 'noxu')"
        )
        return bool(val)
