--
-- FLUX planner size estimates (flux_relation_estimate_size), heap as control.
--
-- Every result is a boolean so the expected output does not depend on
-- platform details; heap's column shows what the shared block estimator
-- gives for the same shape.
--

-- Estimated rows and total cost of a whole-table scan.
CREATE FUNCTION est_plan(tbl regclass, what text) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	plan json;
BEGIN
	EXECUTE format('EXPLAIN (FORMAT JSON) SELECT * FROM %s', tbl) INTO plan;
	RETURN (plan->0->'Plan'->>what)::float8;
END $$;
CREATE FUNCTION est_rows(tbl regclass) RETURNS float8 LANGUAGE sql AS $$
	SELECT est_plan(tbl, 'Plan Rows')
$$;
CREATE FUNCTION real_rows(tbl regclass) RETURNS float8 LANGUAGE plpgsql AS $$
DECLARE
	n float8;
BEGIN
	EXECUTE format('SELECT count(*) FROM %s', tbl) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION real_pages(tbl regclass) RETURNS float8 LANGUAGE sql AS $$
	SELECT pg_relation_size(tbl) / current_setting('block_size')::float8
$$;
-- The estimate is within [0.5x, 2x] of the real row count.
CREATE FUNCTION est_ok(tbl regclass) RETURNS bool LANGUAGE sql AS $$
	SELECT est_rows(tbl) BETWEEN 0.5 * real_rows(tbl) AND 2 * real_rows(tbl)
$$;
-- A never-analyzed table: the rows per page the planner assumes (the
-- estimate over the page count, which is at least 10) is within 10% of the
-- rows a full page really holds (the most rows on any block of a table
-- filled by INSERT alone).
CREATE FUNCTION real_density(tbl regclass) RETURNS float8
LANGUAGE plpgsql AS $$
DECLARE
	n float8;
BEGIN
	EXECUTE format('SELECT max(c) FROM (SELECT count(*) AS c FROM %s
					GROUP BY (ctid::text::point)[0]) s', tbl) INTO n;
	RETURN n;
END $$;
CREATE FUNCTION density_ok(tbl regclass) RETURNS bool LANGUAGE sql AS $$
	SELECT abs(est_rows(tbl) / greatest(real_pages(tbl), 10) /
			   real_density(tbl) - 1) <= 0.10
$$;

CREATE TABLE est_result (shape text, am text, ok bool);
CREATE TABLE est_am (am text);
INSERT INTO est_am VALUES ('heap'), ('flux');

-- Run one statement per access method; %1$s is the AM name.  The
-- statements are not echoed: only est_result is shown.
\set ECHO none
\set per_am 'SELECT format(:''stmt'', am) FROM est_am ORDER BY am DESC \\gexec'

\set stmt 'CREATE TABLE est_empty_%1$s (a int, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'CREATE TABLE est_two_%1$s (a int, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_two_%1$s VALUES (1, 1, 1), (2, 2, 2)'
:per_am
-- A never-vacuumed table is assumed to have at least 10 pages.
\set stmt 'INSERT INTO est_result SELECT ''fresh 2 rows: 10-page rule'', %1$L, est_rows(''est_two_%1$s'') = est_rows(''est_empty_%1$s'') AND est_rows(''est_two_%1$s'') > 100'
:per_am
-- Once vacuumed, an empty table is believed: 0 pages, a zero-cost scan.
\set stmt 'VACUUM est_empty_%1$s'
:per_am
\set stmt 'INSERT INTO est_result SELECT ''empty after VACUUM: 0 pages'', %1$L, est_plan(''est_empty_%1$s'', ''Total Cost'') = 0'
:per_am

-- Never analyzed: narrow rows, wide rows, NULLs, fillfactor 50.
\set stmt 'CREATE TABLE est_narrow_%1$s (a int, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_narrow_%1$s SELECT g, g, g FROM generate_series(1, 5000) g'
:per_am
\set stmt 'CREATE TABLE est_wide_%1$s (a int, n1 name, n2 name, n3 name, n4 name) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_wide_%1$s SELECT g, g::text, g::text, g::text, g::text FROM generate_series(1, 2000) g'
:per_am
\set stmt 'CREATE TABLE est_nulls_%1$s (a int, b int, c bigint, d bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_nulls_%1$s SELECT g, g, CASE WHEN g %% 2 = 0 THEN g END, CASE WHEN g %% 2 = 0 THEN g END FROM generate_series(1, 5000) g'
:per_am
\set stmt 'CREATE TABLE est_ff50_%1$s (a int, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off, fillfactor = 50)'
:per_am
\set stmt 'INSERT INTO est_ff50_%1$s SELECT g, g, g FROM generate_series(1, 5000) g'
:per_am
-- No reloptions at all, so the AM's own default fillfactor applies (a temp
-- table, which autovacuum leaves alone).
\set stmt 'CREATE TEMP TABLE est_ffdef_%1$s (a int, b int, c bigint) USING %1$s'
:per_am
\set stmt 'INSERT INTO est_ffdef_%1$s SELECT g, g, g FROM generate_series(1, 5000) g'
:per_am
\set stmt 'INSERT INTO est_result VALUES (''fresh default fillfactor: density'', %1$L, density_ok(''est_ffdef_%1$s''))'
:per_am
-- 1000 columns: FLUX stores a null bitmap in every tuple, heap only in a
-- tuple that has a NULL.
SELECT format('CREATE TABLE est_cols_%s (%s) USING %s WITH (autovacuum_enabled = off)',
			  am, string_agg('c' || i || ' bool', ', '), am)
FROM est_am, generate_series(1, 1000) i GROUP BY am ORDER BY am DESC \gexec
SELECT format('INSERT INTO est_cols_%s SELECT %s FROM generate_series(1, 300)',
			  am, repeat('true, ', 999) || 'true')
FROM est_am ORDER BY am DESC \gexec
\set stmt 'INSERT INTO est_result VALUES (''fresh 1000 columns: density'', %1$L, density_ok(''est_cols_%1$s''))'
:per_am
\set stmt 'INSERT INTO est_result VALUES (''fresh narrow: density'', %1$L, density_ok(''est_narrow_%1$s'')), (''fresh narrow: rows'', %1$L, est_ok(''est_narrow_%1$s'')), (''fresh wide: density'', %1$L, density_ok(''est_wide_%1$s'')), (''fresh wide: rows'', %1$L, est_ok(''est_wide_%1$s'')), (''fresh with nulls: rows'', %1$L, est_ok(''est_nulls_%1$s'')), (''fresh fillfactor 50: density'', %1$L, density_ok(''est_ff50_%1$s'')), (''fresh fillfactor 50: rows'', %1$L, est_ok(''est_ff50_%1$s''))'
:per_am

-- Analyzed, then grown 3x.
\set stmt 'ANALYZE est_narrow_%1$s'
:per_am
\set stmt 'INSERT INTO est_result SELECT ''after ANALYZE: exact'', %1$L, est_rows(''est_narrow_%1$s'') = real_rows(''est_narrow_%1$s'')'
:per_am
\set stmt 'INSERT INTO est_narrow_%1$s SELECT g, g, g FROM generate_series(5001, 15000) g'
:per_am
\set stmt 'INSERT INTO est_result SELECT ''grown 3x since ANALYZE: rows'', %1$L, est_ok(''est_narrow_%1$s'')'
:per_am

-- Analyzed, then 3 rounds of same-width UPDATE of an unindexed column.
\set stmt 'CREATE TABLE est_upd_%1$s (a int PRIMARY KEY, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_upd_%1$s SELECT g, g, g FROM generate_series(1, 5000) g'
:per_am
\set stmt 'ANALYZE est_upd_%1$s'
:per_am
CREATE TEMP TABLE est_upd_before AS
	SELECT am, est_rows(format('est_upd_%s', am)::regclass) AS est FROM est_am;
\set stmt 'UPDATE est_upd_%1$s SET c = c + 1'
:per_am
:per_am
:per_am
\set stmt 'INSERT INTO est_result VALUES (''3 non-key UPDATE rounds: rows'', %1$L, est_ok(''est_upd_%1$s'')), (''3 non-key UPDATE rounds: no drift'', %1$L, est_rows(''est_upd_%1$s'') = (SELECT est FROM est_upd_before WHERE am = %1$L))'
:per_am

-- Key UPDATEs and DELETEs, then VACUUM.
\set stmt 'CREATE TABLE est_kvac_%1$s (a int PRIMARY KEY, b int, c bigint) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_kvac_%1$s SELECT g, g, g FROM generate_series(1, 5000) g'
:per_am
\set stmt 'UPDATE est_kvac_%1$s SET a = a + 100000 WHERE a %% 2 = 0'
:per_am
\set stmt 'DELETE FROM est_kvac_%1$s WHERE b %% 3 = 0'
:per_am
\set stmt 'VACUUM est_kvac_%1$s'
:per_am
\set stmt 'INSERT INTO est_result SELECT ''key UPDATE + DELETE + VACUUM: rows'', %1$L, est_ok(''est_kvac_%1$s'')'
:per_am

\set ECHO all

SELECT h.shape, h.ok AS heap, f.ok AS flux
FROM est_result h JOIN est_result f USING (shape)
WHERE h.am = 'heap' AND f.am = 'flux'
ORDER BY h.ctid;

-- ANALYZE's average column width is the stored width, as for heap: a
-- compressed value inline, an external value's TOAST pointer.
\set ECHO none
\set stmt 'CREATE TABLE est_width_%1$s (a int, m text, e text) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'ALTER TABLE est_width_%1$s ALTER e SET STORAGE EXTERNAL'
:per_am
\set stmt 'INSERT INTO est_width_%1$s SELECT g, repeat(md5(g::text), 100), repeat(md5(g::text), 100) FROM generate_series(1, 500) g'
:per_am
\set stmt 'ANALYZE est_width_%1$s'
:per_am
\set ECHO all
SELECT attname, h.avg_width = f.avg_width AS same_width,
	   f.avg_width < 100 AS stored_width
FROM pg_stats h JOIN pg_stats f USING (attname)
WHERE h.tablename = 'est_width_heap' AND f.tablename = 'est_width_flux'
ORDER BY attname;

-- Planning a never-analyzed table reads no table block: 50 plans, 0 reads.
\set ECHO none
\set stmt 'CREATE TABLE est_io_%1$s (a int, b int) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_io_%1$s SELECT g, g FROM generate_series(1, 5000) g'
:per_am
\set ECHO all
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
CREATE TEMP TABLE est_io_before AS
	SELECT relname, heap_blks_hit + heap_blks_read AS blks
	FROM pg_statio_user_tables WHERE relname IN ('est_io_heap', 'est_io_flux');
DO $$
BEGIN
	FOR i IN 1..50 LOOP
		EXECUTE 'EXPLAIN SELECT * FROM est_io_heap WHERE a = 1';
		EXECUTE 'EXPLAIN SELECT * FROM est_io_flux WHERE a = 1';
	END LOOP;
END $$;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT s.relname,
	   s.heap_blks_hit + s.heap_blks_read - b.blks AS blocks_read_by_planning
FROM pg_statio_user_tables s JOIN est_io_before b USING (relname)
ORDER BY s.relname;

-- An index scan of a range in physical order visits each table page once, as
-- the planner's index-correlation costing assumes: 3000 rows, 0 to 30 pages.
\set ECHO none
\set stmt 'CREATE TABLE est_corr_%1$s (id int PRIMARY KEY, v int) USING %1$s WITH (autovacuum_enabled = off)'
:per_am
\set stmt 'INSERT INTO est_corr_%1$s SELECT g, g FROM generate_series(1, 20000) g'
:per_am
\set stmt 'ANALYZE est_corr_%1$s'
:per_am
\set ECHO all
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
CREATE TEMP TABLE est_corr_before AS
	SELECT relname, heap_blks_hit + heap_blks_read AS blks
	FROM pg_statio_user_tables WHERE relname IN ('est_corr_heap', 'est_corr_flux');
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT sum(v) FROM est_corr_heap WHERE id BETWEEN 1001 AND 4000;
SELECT sum(v) FROM est_corr_flux WHERE id BETWEEN 1001 AND 4000;
RESET enable_seqscan;
RESET enable_bitmapscan;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT s.relname,
	   s.heap_blks_hit + s.heap_blks_read - b.blks BETWEEN 1 AND 30 AS pages_once
FROM pg_statio_user_tables s JOIN est_corr_before b USING (relname)
ORDER BY s.relname;

\set ECHO none
\set stmt 'DROP TABLE est_empty_%1$s, est_two_%1$s, est_narrow_%1$s, est_wide_%1$s, est_nulls_%1$s, est_ff50_%1$s, est_ffdef_%1$s, est_cols_%1$s, est_upd_%1$s, est_kvac_%1$s, est_io_%1$s, est_width_%1$s, est_corr_%1$s'
:per_am
\set ECHO all
DROP TABLE est_result, est_am;
DROP FUNCTION est_ok, density_ok, real_density, est_rows, est_plan, real_rows,
	real_pages;
