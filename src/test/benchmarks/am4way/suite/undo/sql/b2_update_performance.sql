--
-- B2: Update Performance (OLTP-style targeted operations)
--
-- Measures UNDO overhead for realistic update patterns: single-row PK
-- lookups, small batches, cross-table operations, and one full-table pass.
-- Avoids repeated full-table scans that dominate runtime on small systems.
--
-- Variables: :scenario, :row_count, :create_opts
--

-- ================================================================
-- Setup: two related tables (orders + items)
-- ================================================================
DROP TABLE IF EXISTS bench_items;
DROP TABLE IF EXISTS bench_orders;

CREATE TABLE bench_orders (
    id integer PRIMARY KEY,
    customer_id integer,
    status integer DEFAULT 0,
    total numeric DEFAULT 0,
    updated_at timestamp DEFAULT now()
) :create_opts;

CREATE TABLE bench_items (
    id integer PRIMARY KEY,
    order_id integer,
    quantity integer DEFAULT 1,
    price numeric DEFAULT 9.99
) :create_opts;

INSERT INTO bench_orders (id, customer_id)
SELECT i, (i % 1000) + 1 FROM generate_series(1, :row_count) i;

INSERT INTO bench_items (id, order_id, quantity, price)
SELECT i, ((i - 1) % :row_count) + 1, (i % 10) + 1, round((random() * 100)::numeric, 2)
FROM generate_series(1, :row_count) i;

CREATE INDEX bench_items_order_idx ON bench_items (order_id);

-- Record initial sizes
SELECT pg_relation_size('bench_orders') AS _size \gset
\echo UNDO_BENCH_RESULT|initial_orders_size|bytes|:_size
SELECT pg_relation_size('bench_items') AS _size \gset
\echo UNDO_BENCH_RESULT|initial_items_size|bytes|:_size

-- ================================================================
-- B2a: Single-row UPDATE by PK (1000 individual updates)
-- ================================================================
\echo '--- Single-row UPDATE by PK (1000 rows) ---'

CREATE FUNCTION pg_temp.bench_single_updates(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_orders SET status = status + 1 WHERE id = i;
    END LOOP;
END
$fn$;

SELECT LEAST(:row_count, 1000) AS _n \gset

SELECT clock_timestamp()::text AS _t0 \gset
SELECT pg_temp.bench_single_updates(:_n);
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|single_row_update|time_ms|:_elapsed

-- ================================================================
-- B2b: Small batch UPDATE (10 rows per batch, 100 batches)
-- ================================================================
\echo '--- Batch UPDATE (10-row batches, 100 batches) ---'

CREATE FUNCTION pg_temp.bench_batch_updates(batches integer, batch_sz integer, max_id integer)
RETURNS void LANGUAGE plpgsql AS $fn$
DECLARE
    start_id integer;
BEGIN
    FOR b IN 1..batches LOOP
        start_id := ((b - 1) * batch_sz) % max_id + 1;
        UPDATE bench_orders SET status = status + 1
        WHERE id >= start_id AND id < start_id + batch_sz;
    END LOOP;
END
$fn$;

SELECT clock_timestamp()::text AS _t0 \gset
SELECT pg_temp.bench_batch_updates(100, 10, :row_count);
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|batch_update_10x100|time_ms|:_elapsed

-- ================================================================
-- B2c: Cross-table UPDATE (update order + recalculate from items)
-- ================================================================
\echo '--- Cross-table UPDATE (100 orders with item aggregation) ---'

CREATE FUNCTION pg_temp.bench_cross_table_updates(n integer)
RETURNS void LANGUAGE plpgsql AS $fn$
BEGIN
    FOR i IN 1..n LOOP
        UPDATE bench_items SET quantity = quantity + 1
        WHERE order_id = i AND id = i;

        UPDATE bench_orders SET total = (
            SELECT COALESCE(sum(quantity * price), 0)
            FROM bench_items WHERE order_id = i
        ), updated_at = now()
        WHERE id = i;
    END LOOP;
END
$fn$;

SELECT clock_timestamp()::text AS _t0 \gset
SELECT pg_temp.bench_cross_table_updates(LEAST(:row_count, 100));
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|cross_table_update|time_ms|:_elapsed

-- ================================================================
-- B2d: 1% targeted UPDATE
-- ================================================================
\echo '--- 1% targeted UPDATE ---'

SELECT clock_timestamp()::text AS _t0 \gset
UPDATE bench_orders SET status = status + 1 WHERE id % 100 = 0;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|targeted_1pct_update|time_ms|:_elapsed

-- ================================================================
-- B2e: One full-table pass (for overhead comparison, single round)
-- ================================================================
\echo '--- Single full-table UPDATE ---'

SELECT clock_timestamp()::text AS _t0 \gset
UPDATE bench_orders SET status = status + 1;
SELECT round(extract(epoch FROM (clock_timestamp() - :'_t0'::timestamptz)) * 1000, 2) AS _elapsed \gset
\echo UNDO_BENCH_RESULT|full_table_update_1r|time_ms|:_elapsed

-- Final sizes
SELECT pg_relation_size('bench_orders') AS _size \gset
\echo UNDO_BENCH_RESULT|final_orders_size|bytes|:_size
SELECT pg_relation_size('bench_items') AS _size \gset
\echo UNDO_BENCH_RESULT|final_items_size|bytes|:_size

-- ================================================================
-- Cleanup
-- ================================================================
DROP TABLE bench_items;
DROP TABLE bench_orders;
