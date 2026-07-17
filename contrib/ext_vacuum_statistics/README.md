# ext_vacuum_statistics

Extended vacuum statistics extension for PostgreSQL. It collects and exposes detailed per-table, per-index, and per-database vacuum statistics (buffer I/O, WAL, general, timing) via convenient views in the `ext_vacuum_statistics` schema.

## Installation

```
./configure tmp_install="$(pwd)/my/inst"
make clean && make && make install
cd contrib/ext_vacuum_statistics
make && make install
```

It is essential that the extension is listed in `shared_preload_libraries` because it registers a vacuum hook at server startup.

In your `postgresql.conf`:

```
shared_preload_libraries = 'ext_vacuum_statistics'
```

Restart PostgreSQL.

In your database:

```sql
CREATE EXTENSION ext_vacuum_statistics;
```

## Usage

Query vacuum statistics via the provided views:

```sql
-- Per-table heap vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_tables;

-- Per-index vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_indexes;

-- Per-database aggregate vacuum statistics
SELECT * FROM ext_vacuum_statistics.pg_stats_vacuum_database;
```

Example output:

```
 relname   | wal_records | tuples_deleted | pages_removed
-----------+-------------+----------------+---------------
 mytable   |          15 |            500 |            10
```

Reset statistics when needed:

```sql
SELECT ext_vacuum_statistics.vacuum_statistics_reset();
```

## Configuration (GUCs)

| GUC | Default | Description |
|-----|---------|-------------|
| `vacuum_statistics.enabled` | on | Enable extended vacuum statistics collection |

## Memory usage

Each tracked object (table or index) uses a fixed-size shared memory entry; the exact size depends on the platform.

Example: a database with 1000 tables and 2000 indexes, all tracked, uses about **700 KB** on Ubuntu (3001 entries × 232 bytes). Per-database entries add one entry per tracked database.

## Recipes

**Disable statistics collection temporarily:**

```sql
SET vacuum_statistics.enabled = off;
```

## Views

| View | Description |
|------|-------------|
| `ext_vacuum_statistics.pg_stats_vacuum_tables` | Per-table heap vacuum stats (pages scanned, tuples deleted, dead tuples, etc.) |
| `ext_vacuum_statistics.pg_stats_vacuum_indexes` | Per-index vacuum stats |
| `ext_vacuum_statistics.pg_stats_vacuum_database` | Per-database aggregate vacuum stats |

## Limitations

- Must be loaded via `shared_preload_libraries`; it cannot be loaded on demand.
