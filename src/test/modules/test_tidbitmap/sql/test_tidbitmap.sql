CREATE EXTENSION test_tidbitmap;

--
-- TIDBitmap built directly: several alive at once, set operations, lossy
-- pages, errors.  Every bitmap's contents are checked inside the function;
-- the result is the number of lossy pages (or of blocks, for setop).
--

-- one bitmap, small to large; 20000 blocks grows the pagetable to 32768
-- slots, 60000 grows it past work_mem (131072 slots)
SELECT test_tidbitmap_check(1, 1, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 100, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 2000, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 20000, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 20000, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 60000, 4096, 0, -1);
SELECT test_tidbitmap_check(1, 100, 4096, 0, -1);

-- several bitmaps growing together; the first is freed half-way
SELECT test_tidbitmap_check(2, 20000, 4096, 0, -1);
SELECT test_tidbitmap_check(5, 20000, 4096, 0, -1);
-- (how many pages go lossy depends on the pointer size)
SELECT test_tidbitmap_check(3, 20000, 1024, 0, -1) > 0 AS lossy;

-- lossy: every 7th block added whole, and a bitmap over its memory limit
SELECT test_tidbitmap_check(1, 20000, 4096, 7, -1);
SELECT test_tidbitmap_check(1, 20000, 64, 0, -1) > 0 AS lossy;
SELECT test_tidbitmap_check(3, 20000, 64, 0, -1) > 0 AS lossy;
SELECT test_tidbitmap_check(1, 20000, 4096, 0, -1);

-- BitmapAnd and BitmapOr operations
SELECT test_tidbitmap_setop('intersect', 20000);
SELECT test_tidbitmap_setop('union', 20000);

-- an error in the middle of building, with one or several bitmaps alive
SELECT test_tidbitmap_check(1, 20000, 4096, 0, 15000);
SELECT test_tidbitmap_check(3, 20000, 4096, 0, 15000);
SELECT test_tidbitmap_check(3, 20000, 4096, 0, 5000);
SELECT test_tidbitmap_check(1, 20000, 4096, 0, -1);
SELECT test_tidbitmap_check(3, 20000, 4096, 0, -1);

-- errors in subtransactions, then more bitmaps in the same transaction
BEGIN;
DO $$
BEGIN
	FOR i IN 1..20 LOOP
		BEGIN
			PERFORM test_tidbitmap_check(1 + i % 3, 20000, 4096, 0, 15000);
		EXCEPTION WHEN others THEN
			NULL;
		END;
	END LOOP;
END
$$;
SELECT test_tidbitmap_check(2, 20000, 4096, 0, -1);
COMMIT;

--
-- The same through the executor.  About 12 rows per heap page, so a bitmap
-- matching one row in 11 covers every page of the table.
--
CREATE TABLE tbm_t (id int, a int, b int, c int, t tsvector)
  WITH (fillfactor = 10, autovacuum_enabled = off);
INSERT INTO tbm_t
  SELECT i, i % 11, i % 13, i % 17, to_tsvector('simple', 'w' || i % 50)
  FROM generate_series(1, 30000) i;
CREATE INDEX tbm_t_a ON tbm_t (a);
CREATE INDEX tbm_t_b ON tbm_t (b);
CREATE INDEX tbm_t_c ON tbm_t (c);
CREATE INDEX tbm_t_t ON tbm_t USING gin (t);
VACUUM ANALYZE tbm_t;
SELECT pg_relation_size('tbm_t') / current_setting('block_size')::int > 2000
  AS enough_pages;

-- what a backend keeps between queries stays under work_mem
CREATE FUNCTION tbm_cache_bounded() RETURNS bool LANGUAGE sql AS $$
  SELECT count(*) <= 1 AND
    coalesce(sum(total_bytes), 0) <=
      pg_size_bytes(current_setting('work_mem')) + 16384
  FROM pg_backend_memory_contexts WHERE name = 'TIDBitmap cache'
$$;

CREATE FUNCTION tbm_has_lossy(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE
	l text;
BEGIN
	FOR l IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q
	LOOP
		IF l LIKE '%lossy=%' THEN
			RETURN true;
		END IF;
	END LOOP;
	RETURN false;
END
$$;

CREATE FUNCTION tbm_slow(int) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
	PERFORM pg_sleep(0.001);
	RETURN true;
END
$$;

-- the answers without bitmaps, to compare with
SET enable_bitmapscan = off;
SET enable_indexscan = off;
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT count(*) FROM tbm_t WHERE a = 0 AND b = 0;
SELECT count(*) FROM tbm_t WHERE (a = 0 AND b = 0) OR c = 0;
SELECT count(*) FROM tbm_t WHERE t @@ 'w1:*'::tsquery;
SELECT count(*) FROM tbm_t WHERE t @@ 'w1:* | w2:*'::tsquery;
SELECT count(*) FROM tbm_t t1 JOIN tbm_t t2 USING (id) WHERE t1.a = 0 AND t2.b = 0;
SELECT count(*) FROM (VALUES (0), (1), (2)) v(x) JOIN tbm_t ON tbm_t.a = v.x;
RESET enable_bitmapscan;

SET enable_seqscan = off;
SET max_parallel_workers_per_gather = 0;

SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT tbm_cache_bounded();

-- two bitmaps alive at once
EXPLAIN (COSTS OFF) SELECT count(*) FROM tbm_t WHERE a = 0 AND b = 0;
SELECT count(*) FROM tbm_t WHERE a = 0 AND b = 0;
EXPLAIN (COSTS OFF) SELECT count(*) FROM tbm_t WHERE (a = 0 AND b = 0) OR c = 0;
SELECT count(*) FROM tbm_t WHERE (a = 0 AND b = 0) OR c = 0;

-- GIN partial match: a bitmap per prefix, then the scan's own bitmap
EXPLAIN (COSTS OFF) SELECT count(*) FROM tbm_t WHERE t @@ 'w1:*'::tsquery;
SELECT count(*) FROM tbm_t WHERE t @@ 'w1:*'::tsquery;
SELECT count(*) FROM tbm_t WHERE t @@ 'w1:* | w2:*'::tsquery;

-- a bitmap heap scan on each side of a join
SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM tbm_t t1 JOIN tbm_t t2 USING (id) WHERE t1.a = 0 AND t2.b = 0;
SELECT count(*) FROM tbm_t t1 JOIN tbm_t t2 USING (id) WHERE t1.a = 0 AND t2.b = 0;
RESET enable_nestloop;

-- rescans: the inner bitmap is freed and rebuilt for each outer row
SET enable_hashjoin = off;
SET enable_material = off;
EXPLAIN (COSTS OFF)
SELECT count(*) FROM (VALUES (0), (1), (2)) v(x) JOIN tbm_t ON tbm_t.a = v.x;
SELECT count(*) FROM (VALUES (0), (1), (2)) v(x) JOIN tbm_t ON tbm_t.a = v.x;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- an open cursor keeps its bitmap while other queries run
BEGIN;
DECLARE c CURSOR FOR SELECT id FROM tbm_t WHERE a = 0;
MOVE 10 IN c;
SELECT count(*) FROM tbm_t WHERE b = 0;
SELECT count(*) FROM tbm_t WHERE a = 0 AND b = 0;
MOVE ALL IN c;
COMMIT;

-- an error in the heap scan, after the bitmap is built
SELECT count(*) FROM tbm_t WHERE a = 0 AND 1 / (id - 15004) > -2;
SELECT count(*) FROM tbm_t WHERE a = 0;

-- the same in a subtransaction, then a bitmap in the same transaction
BEGIN;
DO $$
DECLARE
	n int;
BEGIN
	FOR i IN 1..5 LOOP
		BEGIN
			SELECT count(*) INTO n FROM tbm_t WHERE a = 0 AND 1 / (id - 15004) > -2;
		EXCEPTION WHEN division_by_zero THEN
			NULL;
		END;
	END LOOP;
END
$$;
SELECT count(*) FROM tbm_t WHERE a = 0;
COMMIT;
SELECT tbm_cache_bounded();

-- cancel (terse: the timeout can fire inside or outside pg_sleep)
\set VERBOSITY terse
SET statement_timeout = '100ms';
SELECT count(*) FROM tbm_t WHERE a = 0 AND tbm_slow(id);
RESET statement_timeout;
\set VERBOSITY default
SELECT count(*) FROM tbm_t WHERE a = 0;

-- work_mem lowered: the bitmap goes lossy, and the kept memory follows
SET work_mem = '64kB';
SELECT tbm_has_lossy('SELECT count(*) FROM tbm_t WHERE a = 0');
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT tbm_cache_bounded();
SELECT count(*) FROM tbm_t WHERE a = 0 AND b = 0;
-- raised again
SET work_mem = '64MB';
SELECT tbm_has_lossy('SELECT count(*) FROM tbm_t WHERE a = 0');
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT tbm_cache_bounded();
RESET work_mem;
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT tbm_cache_bounded();

-- parallel bitmap heap scan: its bitmap is in shared memory
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET max_parallel_workers_per_gather = 2;
SET cpu_tuple_cost = 1;
EXPLAIN (COSTS OFF) SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT count(*) FROM tbm_t WHERE a = 0;
RESET cpu_tuple_cost;
RESET max_parallel_workers_per_gather;
RESET min_parallel_table_scan_size;
RESET parallel_tuple_cost;
RESET parallel_setup_cost;

-- a new backend
\c -
SET enable_seqscan = off;
SET enable_indexscan = off;
SET max_parallel_workers_per_gather = 0;
SELECT count(*) FROM tbm_t WHERE a = 0;
SELECT test_tidbitmap_check(1, 20000, 4096, 0, -1);
SELECT tbm_cache_bounded();
