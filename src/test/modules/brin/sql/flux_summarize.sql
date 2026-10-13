--
-- BRIN summarization of one block range on a FLUX table, with a heap twin.
--
-- VACUUM, brin_summarize_new_values and brin_summarize_range summarize a
-- range through table_index_build_range_scan with that range's blocks; the
-- table AM must scan only those.  Each summary item must then equal the
-- minimum and maximum of the rows in its range, there must be one item per
-- range, and a BRIN bitmap scan must find every row.
--
CREATE EXTENSION pageinspect;

-- Items of a BRIN index on column v that are not the min/max of the rows in
-- their range ("wrong"), and items beyond one per range ("dup").
CREATE FUNCTION brin_check(tbl regclass, idx regclass, ppr int,
						   OUT wrong bigint, OUT dup bigint)
LANGUAGE plpgsql AS $$
BEGIN
	EXECUTE format($q$
		WITH items AS (
			SELECT i.blknum, i.value
			  FROM generate_series(1, (pg_relation_size(%2$L::regclass) /
									   current_setting('block_size')::int)::int - 1) p,
				   LATERAL get_raw_page(%2$L, p) g(pg),
				   LATERAL brin_page_items(g.pg, %2$L::regclass) i
			 WHERE brin_page_type(g.pg) = 'regular' AND NOT i.placeholder),
		ranges AS (
			SELECT (ctid::text::point)[0]::bigint / %3$s * %3$s AS blknum,
				   '{' || min(v) || ' .. ' || max(v) || '}' AS value
			  FROM %1$s GROUP BY 1)
		SELECT (SELECT count(*) FROM (SELECT * FROM items
									  EXCEPT ALL SELECT * FROM ranges) x),
			   (SELECT count(*) - count(DISTINCT blknum) FROM items)$q$,
		tbl, idx, ppr)
	INTO wrong, dup;
END $$;

-- The text key of row g.
CREATE FUNCTION brin_key(g int) RETURNS text IMMUTABLE LANGUAGE sql
	RETURN lpad(g::text, 8, '0') || md5(g::text) || md5(g::text || 'a');

-- Text keys, ranges summarized by VACUUM: a varlena summary does not fit in
-- place, so a summary of the wrong blocks lands elsewhere and the revmap
-- points at it.
CREATE TABLE brin_t_heap (k int, v text) USING heap;
CREATE TABLE brin_t_flux (k int, v text) USING flux;
CREATE INDEX brin_t_heap_idx ON brin_t_heap USING brin (v) WITH (pages_per_range = 4);
CREATE INDEX brin_t_flux_idx ON brin_t_flux USING brin (v) WITH (pages_per_range = 4);
INSERT INTO brin_t_heap SELECT g, brin_key(g) FROM generate_series(1, 20000) g;
INSERT INTO brin_t_flux SELECT g, brin_key(g) FROM generate_series(1, 20000) g;
VACUUM brin_t_heap, brin_t_flux;
SELECT * FROM brin_check('brin_t_heap', 'brin_t_heap_idx', 4);
SELECT * FROM brin_check('brin_t_flux', 'brin_t_flux_idx', 4);

SET enable_seqscan = off;
SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT k FROM brin_t_flux WHERE v = brin_key(300);
SELECT count(*) AS missed FROM generate_series(100, 20000, 100) x
 WHERE NOT EXISTS (SELECT FROM brin_t_heap t WHERE t.v = brin_key(x));
SELECT count(*) AS missed FROM generate_series(100, 20000, 100) x
 WHERE NOT EXISTS (SELECT FROM brin_t_flux t WHERE t.v = brin_key(x));
RESET enable_seqscan;
RESET enable_indexscan;

-- Integer keys, one range desummarized and summarized again.
CREATE TABLE brin_i_heap (v int) USING heap;
CREATE TABLE brin_i_flux (v int) USING flux;
INSERT INTO brin_i_heap SELECT generate_series(1, 20000);
INSERT INTO brin_i_flux SELECT generate_series(1, 20000);
CREATE INDEX brin_i_heap_idx ON brin_i_heap USING brin (v) WITH (pages_per_range = 1);
CREATE INDEX brin_i_flux_idx ON brin_i_flux USING brin (v) WITH (pages_per_range = 1);
SELECT brin_desummarize_range('brin_i_heap_idx', 10);
SELECT brin_desummarize_range('brin_i_flux_idx', 10);
SELECT brin_summarize_range('brin_i_heap_idx', 10);
SELECT brin_summarize_range('brin_i_flux_idx', 10);
SELECT * FROM brin_check('brin_i_heap', 'brin_i_heap_idx', 1);
SELECT * FROM brin_check('brin_i_flux', 'brin_i_flux_idx', 1);

-- New ranges, summarized by brin_summarize_new_values.
INSERT INTO brin_i_heap SELECT generate_series(20001, 30000);
INSERT INTO brin_i_flux SELECT generate_series(20001, 30000);
SELECT brin_summarize_new_values('brin_i_heap_idx') > 0 AS summarized;
SELECT brin_summarize_new_values('brin_i_flux_idx') > 0 AS summarized;
SELECT * FROM brin_check('brin_i_heap', 'brin_i_heap_idx', 1);
SELECT * FROM brin_check('brin_i_flux', 'brin_i_flux_idx', 1);

SET enable_seqscan = off;
SET enable_indexscan = off;
SELECT count(*) AS missed FROM generate_series(1, 30000, 97) x
 WHERE NOT EXISTS (SELECT FROM brin_i_heap t WHERE t.v = x);
SELECT count(*) AS missed FROM generate_series(1, 30000, 97) x
 WHERE NOT EXISTS (SELECT FROM brin_i_flux t WHERE t.v = x);
RESET enable_seqscan;
RESET enable_indexscan;

DROP TABLE brin_t_heap, brin_t_flux, brin_i_heap, brin_i_flux;
DROP FUNCTION brin_check, brin_key;
DROP EXTENSION pageinspect;
