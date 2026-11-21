# pgtrashcan

`pgtrashcan` is a PostgreSQL extension that intercepts `DROP TABLE` and moves
the relation to a reserved `pgtrashcan` schema instead of immediately
destroying it. The table can later be restored ("flashed back") to its
original schema, or permanently deleted ("purged").

It is loosely modelled on Oracle's recyclebin (`FLASHBACK TABLE ... TO BEFORE
DROP`) and Snowflake's / BigQuery's `UNDROP TABLE`.

This extension was originally written by Peter Eisentraut in 2014. The
present fork extends it with `CASCADE` handling, a restore / purge API,
JSONB metadata capture for objects that cannot safely be moved
(foreign keys, triggers, views, rules, RLS policies), and a 58-test
regression suite.

## Build and install

```
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config
make USE_PGXS=1 PG_CONFIG=/path/to/pg_config install
```

Inside the PostgreSQL source tree, drop this directory into `contrib/` and
plain `make` works.

`pgtrashcan` installs a `ProcessUtility_hook`, so the shared library must be
preloaded:

```
# postgresql.conf
shared_preload_libraries = 'pgtrashcan'
```

Restart the server, then in each database that should use it:

```
CREATE EXTENSION pgtrashcan;
```

## A short tour

```sql
-- Drop something
postgres=# CREATE TABLE inventory (id int primary key, item text);
postgres=# INSERT INTO inventory VALUES (1, 'router');
postgres=# DROP TABLE inventory;

-- It's not gone -- it's in the pgtrashcan schema
postgres=# SELECT trash_name, orig_schema, orig_name, drop_time
           FROM pgtrashcan_list_dropped();
   trash_name    | orig_schema | orig_name |          drop_time
-----------------+-------------+-----------+------------------------------
 trash_$16385    | public      | inventory | 2026-06-29 14:12:33.421+00

-- Restore it
postgres=# SELECT pgtrashcan_restore('inventory');
postgres=# SELECT * FROM inventory;
 id |  item
----+--------
  1 | router

-- Or purge it permanently
postgres=# DROP TABLE inventory;
postgres=# SELECT pgtrashcan_purge('inventory');
```

## What survives the drop, what does not

Moved to `pgtrashcan` with the table and fully restored on flashback:

- the table heap (including TOAST)
- indexes (including the primary key)
- sequences owned by the table (`SERIAL` / `GENERATED ... AS IDENTITY`)

Permanently dropped at `DROP` time; definitions captured into
`pgtrashcan._trash_catalog.metadata` (JSONB) and emitted as `NOTICE` on
restore for manual reconstruction:

- foreign-key constraints (both directions)
- triggers
- views and materialized views that referenced the table
- rules
- RLS policies

`DROP TABLE ... CASCADE` walks `pg_depend` to also move FK-dependent child
tables. `DROP TABLE ... RESTRICT` (the default) errors out if dependents
exist, matching standard PostgreSQL behaviour.

Temporary tables, partitioned tables, and foreign tables are not handled by
the extension and pass through to the standard `DROP` path.

## Configuration

| GUC                              | Default | Scope         | Purpose                                              |
|----------------------------------|---------|---------------|------------------------------------------------------|
| `pgtrashcan.enabled`             | `on`    | `PGC_USERSET` | Per-session opt-out; mirrors Oracle's `recyclebin`.  |
| `pgtrashcan.internal_operation`  | `off`   | `PGC_SUSET`   | Internal-only; gates catalog DML for restore/purge.  |

## Full reference

See `doc/src/sgml/pgtrashcan.sgml` for the complete reference: catalog
schema, every SQL function, behaviour edge cases, limitations.

## Running the regression suite

```
make USE_PGXS=1 installcheck PG_CONFIG=/path/to/pg_config
```

Requires `shared_preload_libraries = 'pgtrashcan'` in the target cluster's
`postgresql.conf` and a server restart.

## Credits

- Peter Eisentraut, original 2014 `pgtrashcan` (the hook scaffolding and the
  trash-can UX this extension still builds on).
