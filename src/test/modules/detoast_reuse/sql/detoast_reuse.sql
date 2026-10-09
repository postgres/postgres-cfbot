-- Detoasting a column once per row when several expressions reference it.
--
-- The detoasted copy is kept beside the slot (tts_detoasted); tts_values keeps
-- the stored datum.  Only argument positions of functions and operators read
-- the copy, so anything that stores rows, passes the column on whole, or
-- inspects its stored form sees the toast pointer without needing a rule for
-- it.  The cases below pin the number of detoasts per query shape and, where
-- a pointer must survive, that it does.
--
-- No statement here forces JIT: sanitizer builds crash inside LLVM on any
-- forced JIT compilation.  Run the whole file with jit = on and the three
-- cost settings at 0 via PG_TEST_INITDB_EXTRA_OPTS to cover the JIT-compiled
-- form of the new expression steps; jit defaults to off, so the costs alone
-- change nothing.
--
-- detoast_attr() runs an injection point whenever it fetches an out-of-line
-- value or decompresses an inline one, so with the points attached in notice
-- mode the number of NOTICE lines after a statement is the number of detoasts
-- it performed.

CREATE TABLE sd (id int PRIMARY KEY, doc jsonb, small jsonb, txt text, ctxt text);
-- doc and txt out of line and uncompressed; ctxt compressed but inline
ALTER TABLE sd ALTER COLUMN doc SET STORAGE EXTERNAL,
               ALTER COLUMN txt SET STORAGE EXTERNAL,
               ALTER COLUMN ctxt SET COMPRESSION pglz;
INSERT INTO sd
SELECT 1,
       '{"a": 1, "b": 2, "c": 3}'::jsonb
         || (SELECT jsonb_object_agg('k' || i, md5(i::text) || repeat(md5((i * 3)::text), 8))
             FROM generate_series(1, 200) i),
       '{"a": 1, "b": 2}',
       'abc' || repeat(md5('x'), 200),
       repeat('x', 50000);
-- a second copy with several rows, for the parallel case below
CREATE TABLE sdpar (id int, doc jsonb);
ALTER TABLE sdpar ALTER COLUMN doc SET STORAGE EXTERNAL;
INSERT INTO sdpar SELECT i, doc FROM sd, generate_series(1, 4) i;
VACUUM ANALYZE sd, sdpar;
SELECT pg_column_size(doc) > 8192 AS doc_external,
       pg_column_toast_chunk_id(doc) IS NOT NULL AS doc_has_chunks,
       pg_column_toast_chunk_id(ctxt) IS NULL AS ctxt_inline,
       pg_column_compression(ctxt) AS ctxt_compression
FROM sd;

CREATE EXTENSION injection_points;
-- attached for the whole instance rather than this backend, so that the
-- detoasts a parallel worker performs are counted as well; every statement
-- but the parallel case below runs without workers, since some CI runs
-- default to debug_parallel_query = regress and that would split the counts
-- across processes for no purpose
SET debug_parallel_query = off;
SELECT injection_points_attach('detoast-attr-external', 'notice');
SELECT injection_points_attach('detoast-attr-compressed', 'notice');

-- one reference: one detoast
SELECT doc->'a' FROM sd;
-- two references in the target list: one detoast
SELECT doc->'a', doc->'b' FROM sd;
-- eight mixed operators: one detoast
SELECT doc->'a', doc->>'b', doc ? 'c', doc @> '{"a": 1}', doc->'b', doc->>'c', doc ? 'a', doc @> '{"c": 3}' FROM sd;
-- references in WHERE and in the target list: one detoast
SELECT doc->'a' FROM sd WHERE doc ? 'b' AND doc @> '{"c": 3}';
-- lazy: the first predicate fails, so the row is detoasted once and never again
SELECT id FROM sd WHERE doc ? 'zzz' AND doc @> '{"c": 3}';
-- a chained operator counts once for the inner Var
SELECT doc->'a'->'x', doc->'a'->'y' FROM sd;

-- functions that inspect the stored form get the stored datum while the
-- other references share: one detoast, stored sizes reported
SELECT pg_column_size(doc) > 8192 AS stored_size, pg_column_compression(doc) IS NULL AS uncompressed,
       doc->'a', doc->'b' FROM sd;
-- slice and size readers do not count as detoasting references: no detoast at all
SELECT octet_length(txt), substr(txt, 1, 3), starts_with(txt, 'abc'), left(txt, 3) FROM sd;
-- a compressed inline value is decompressed once for two full readers
SELECT length(md5(ctxt)), ctxt = ctxt FROM sd;

-- a bare Var projected under a Sort stores the toast pointer, and the
-- expressions still share: one detoast
WITH s AS MATERIALIZED (SELECT doc->'a' AS a, doc->'b' AS b, doc AS d FROM sd ORDER BY id)
SELECT a, b, pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept FROM s;
-- a CTE scan over a materialized toast pointer: one detoast
WITH d AS MATERIALIZED (SELECT doc FROM sd) SELECT doc->'a', doc->'b' FROM d;
-- a scan inside a correlated subplan: one detoast
SELECT (SELECT q.doc->'a' || q.doc->'b' FROM sd q WHERE q.id = p.id) FROM sd p;
-- through LockRows: one detoast
SELECT doc->'a', doc->'b' FROM sd FOR UPDATE;
-- an UPDATE whose WHERE references the column twice detoasts once and keeps
-- the toast pointer in the new tuple
CREATE TEMP TABLE before AS SELECT pg_column_toast_chunk_id(doc) AS chunk FROM sd;
UPDATE sd SET small = small WHERE doc ? 'a' AND doc @> '{"b": 2}';
SELECT pg_column_toast_chunk_id(doc) = (SELECT chunk FROM before) AS pointer_kept FROM sd;
-- a parallel worker detoasts once per row like the leader: four rows give
-- four detoasts however they are split between the two processes, where one
-- detoast per reference would give eight
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0; SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
EXPLAIN (COSTS OFF) SELECT count(*) FROM sdpar WHERE doc ? 'a' AND doc @> '{"b": 2}';
SELECT count(*) FROM sdpar WHERE doc ? 'a' AND doc @> '{"b": 2}';
RESET max_parallel_workers_per_gather; RESET parallel_setup_cost;
RESET parallel_tuple_cost; RESET min_parallel_table_scan_size;
-- joins: the expressions are evaluated at the join and the copy is kept
-- beside the child's slot; hash join (probe side), nested loop (both sides)
-- and both sides of a merge join detoast once
CREATE TABLE sd2 (id int PRIMARY KEY, doc jsonb);
ALTER TABLE sd2 ALTER COLUMN doc SET STORAGE EXTERNAL;
INSERT INTO sd2 SELECT id, doc FROM sd;
SET enable_nestloop = off; SET enable_mergejoin = off;
SELECT p.doc->'a', p.doc->'b' FROM sd p JOIN sd2 q ON p.id = q.id;
SELECT p.doc->'a', p.doc->'b', q.doc->'a', q.doc->'b' FROM sd p JOIN sd2 q ON p.id = q.id;
RESET enable_nestloop; RESET enable_mergejoin;
SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT p.doc->'a', p.doc->'b', q.doc->'a', q.doc->'b' FROM sd p JOIN sd2 q ON p.id = q.id;
RESET enable_hashjoin; RESET enable_mergejoin;
SET enable_hashjoin = off; SET enable_nestloop = off;
SELECT p.doc->'a', p.doc->'b', q.doc->'a', q.doc->'b' FROM sd p JOIN sd2 q ON p.id = q.id;
RESET enable_hashjoin; RESET enable_nestloop;
-- a projecting child carries its copy along with the stored datum, so a
-- join reading the child's slot directly finds it: one detoast for the
-- scan's quals and the join's expressions together
SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT p.doc->'a', p.doc->'b' FROM sd p JOIN sd2 q ON p.id = q.id WHERE p.doc ? 'a' AND p.doc @> '{"b": 2}';
RESET enable_hashjoin; RESET enable_mergejoin;
-- a hash join key on the probe side is hashed and then compared from one
-- copy, since any outer-side reference counts at a join; the hashed side is
-- hashed once when the table is built and compared per match from the
-- stored tuple, and the scan's quals share among themselves (four detoasts:
-- scan quals, table build, probe key, match)
SET enable_nestloop = off; SET enable_mergejoin = off;
SELECT count(*) FROM sd p JOIN sd2 q ON p.doc = q.doc WHERE p.doc ? 'a' AND p.doc @> '{"b": 2}';
RESET enable_nestloop; RESET enable_mergejoin;
-- references split between a scan and the join above it add up as well,
-- on either side: one detoast
SET enable_hashjoin = off; SET enable_mergejoin = off;
SELECT q.doc->>'a' FROM sd p JOIN sd2 q ON p.id = q.id WHERE q.doc ? 'a';
RESET enable_hashjoin; RESET enable_mergejoin;
-- a merge join key that a join filter references again is compared from the
-- same copy: one detoast per side
SET enable_nestloop = off; SET enable_hashjoin = off;
SELECT count(*) FROM sd p JOIN sd2 q ON p.doc = q.doc AND p.doc ? (q.doc->>'k1');
RESET enable_nestloop; RESET enable_hashjoin;
-- an ancestor reading a column the join projects bare sees the stored form
-- (the bare column comes last in the target list, after the expressions
-- that detoast it)
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept, x
FROM (SELECT (p.doc->>'a')::int + (p.doc->>'b')::int + (q.doc->>'a')::int AS x, p.doc AS d
      FROM sd p JOIN sd2 q ON p.id = q.id OFFSET 0) s;
-- an outer column passed down as a nestloop parameter goes down as the
-- stored pointer (Memoize keeps it as a cache key) while the outer quals
-- share; both with a parent that stores rows and with one that does not
CREATE INDEX sd2_doc_hash ON sd2 USING hash (doc);
SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_seqscan = off;
SELECT count(*) FROM sd o JOIN sd2 q ON q.doc = o.doc WHERE o.doc ? 'a' AND o.doc @> '{"b": 2}';
SELECT (q.doc->>'a')::int FROM sd o JOIN sd2 q ON q.doc = o.doc WHERE o.doc ? 'a' AND o.doc @> '{"b": 2}';
RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_seqscan;
DROP TABLE sd2;
-- a scan without projection under a parent that stores its rows (Sort,
-- hashed Agg) detoasts once; the parent copies the tuple, not the copy
WITH s AS MATERIALIZED (SELECT * FROM sd WHERE doc ? 'a' AND doc @> '{"b": 2}' ORDER BY id)
SELECT count(*) FROM s;
SET enable_sort = off;
SELECT count(*) FROM sd WHERE doc ? 'a' AND doc @> '{"b": 2}' GROUP BY id;
-- a hashed grouping column is copied out of the slot as the stored pointer
-- (its hash is computed from the stored datum, hence one more detoast)
SELECT count(*) FROM sd WHERE doc ? 'a' AND doc @> '{"b": 2}' GROUP BY doc;
-- an aggregated column the hashed Agg would spill by value is the stored
-- pointer too, while the aggregate argument shares the copy the scan's two
-- references made: one detoast
SELECT id, sum((doc->>'a')::int) FROM sd WHERE doc ? 'a' AND doc @> '{"b": 2}' GROUP BY id;
RESET enable_sort;
-- aggregate arguments referencing the same input column detoast it once
SELECT sum((doc->>'a')::int), sum((doc->>'b')::int) FROM sd;
-- references split between the scan and the aggregate add up: the scan's
-- single filter reference makes the copy and the aggregate argument finds
-- it, except for the first row of a plain or sorted aggregate group, which
-- the Agg reads from a copied tuple (two detoasts here, for one row)
SELECT sum((doc->>'a')::int) FROM sd WHERE doc ? 'a';
-- an aggregate taking the column whole gets the stored pointer, the others
-- still share: one detoast
SELECT sum((doc->>'a')::int), sum((doc->>'b')::int), count(doc) FROM sd;
-- a function inspecting the stored form in an ancestor sees it (bare column
-- last, see above): one detoast, pointer kept
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept, a, b
FROM (SELECT doc->'a' AS a, doc->'b' AS b, doc AS d FROM sd OFFSET 0) s;
-- the same through an Append
CREATE TABLE sdp (id int, doc jsonb) PARTITION BY RANGE (id);
CREATE TABLE sdp1 PARTITION OF sdp FOR VALUES FROM (0) TO (10);
CREATE TABLE sdp2 PARTITION OF sdp FOR VALUES FROM (10) TO (20);
ALTER TABLE sdp ALTER COLUMN doc SET STORAGE EXTERNAL;
INSERT INTO sdp SELECT i, doc FROM sd, (VALUES (1), (12)) v(i);
VACUUM ANALYZE sdp;
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept, a, b
FROM (SELECT doc->'a' AS a, doc->'b' AS b, doc AS d FROM sdp OFFSET 0) s;
-- references split between partition scans and a hashed aggregate above
-- the Append add up too: one detoast per row
SET enable_sort = off;
SELECT count(*) FROM (SELECT id, sum((doc->>'a')::int) FROM sdp WHERE doc ? 'a' GROUP BY id) g;
RESET enable_sort;
-- partition scans under a storing parent: one detoast per row
WITH s AS MATERIALIZED (SELECT * FROM sdp WHERE doc ? 'a' AND doc @> '{"b": 2}' ORDER BY id)
SELECT count(*) FROM s;
DROP TABLE sdp;
-- a column handed to a correlated subplan as a parameter goes down as the
-- stored pointer: one detoast for the two quals, pointer kept inside
SELECT (SELECT pg_column_toast_chunk_id(p.doc) IS NOT NULL) AS pointer_kept
FROM sd p WHERE p.doc ? 'a' AND p.doc @> '{"b": 2}';
-- but a subplan reading the parameter as a function argument finds the
-- outer row's copy: one detoast for the outer quals and the subplan together
SELECT count(*) FROM sd p WHERE p.doc ? 'a' AND p.doc @> '{"b": 2}' AND EXISTS (SELECT 1 FROM jsonb_each_text(p.doc) e WHERE e.value = '1');
-- a subplan reading the parameter once per inner row makes the copy itself
-- and detoasts once per outer row, not per inner row
SELECT count(*) FROM sd p WHERE EXISTS (SELECT 1 FROM generate_series(1, 3) g WHERE p.doc ? ('k' || g));
-- an outer column referenced once in a join filter is detoasted once per
-- outer row, not once per inner row: here the EXISTS is pulled up into a
-- semi join whose filter references the outer column against each of the
-- five inner rows (two detoasts per outer row, the scan's own filter and
-- the join's)
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*) FROM sd t WHERE EXISTS (SELECT 1 FROM generate_series(1, 5) g WHERE t.doc ? ('k' || g) AND t.doc ? 'k1');
SELECT count(*) FROM sd t WHERE EXISTS (SELECT 1 FROM generate_series(1, 5) g WHERE t.doc ? ('k' || g) AND t.doc ? 'k1');
-- the same for a nestloop parameter
SELECT count(*) FROM sd o, LATERAL (SELECT count(*) FROM generate_series(1, 3) g WHERE o.doc ? ('k' || g) AND o.doc @> '{"b": 2}') s;
-- a subplan inside a subplan: the forwarding address is only recorded when
-- the subplan's argument is a plain Var of a slot, so at the second level,
-- where the argument is itself a parameter, the inner subplan makes its own
-- copy (two detoasts for the one row rather than one)
SELECT count(*) FROM sd p
 WHERE p.doc ? 'a'
   AND EXISTS (SELECT 1 FROM generate_series(1, 2) g
                WHERE EXISTS (SELECT 1 FROM jsonb_each_text(p.doc) e
                               WHERE e.value = '1'));
-- two levels of nestloop parameter, where each level takes the column from
-- the same outer slot rather than from the level above: both find the one
-- copy (one detoast)
SELECT count(*) FROM sd p,
     LATERAL (SELECT g FROM generate_series(1, 2) g WHERE p.doc ? 'a') x,
     LATERAL (SELECT h FROM generate_series(1, 2) h WHERE p.doc ? ('k' || x.g)) y;
-- a parameterised inner plan that is rescanned: the copy belongs to the
-- outer row, so each outer row detoasts once however often the inner side
-- is restarted
SELECT count(*) FROM sd p, LATERAL (SELECT count(*) FROM generate_series(1, 3) g
                                     WHERE p.doc ? ('k' || g) AND p.doc @> '{"b": 2}') s;
-- a receiver that keeps the rows (here SPI, via a set-returning function) gets
-- toast pointers, not full values, and the scan still shares
CREATE FUNCTION sd_rows() RETURNS TABLE (d jsonb, a jsonb, b jsonb) LANGUAGE plpgsql AS $$
BEGIN RETURN QUERY SELECT doc, doc->'a', doc->'b' FROM sd; END $$;
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept, a, b FROM sd_rows();
DROP FUNCTION sd_rows();
-- an aggregate that keeps its argument keeps the stored pointer
SELECT count(DISTINCT doc) FROM sd WHERE doc ? 'a' AND doc @> '{"b": 2}';
-- a column handed on whole through RelabelType, CASE, COALESCE, GREATEST or
-- NULLIF is projected like a plain Var: the pointer is kept and the other
-- references share
SELECT pg_column_toast_chunk_id(t) IS NOT NULL AS pointer_kept
FROM (SELECT txt COLLATE "C" AS t FROM sd WHERE txt LIKE 'abc%' AND txt LIKE '%a6' OFFSET 0) s;
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept
FROM (SELECT CASE WHEN id > 0 THEN doc END AS d FROM sd WHERE doc ? 'a' AND doc ? 'b' OFFSET 0) s;
SELECT pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept, pg_column_toast_chunk_id(n) IS NOT NULL AS pointer_kept2
FROM (SELECT GREATEST(doc, '{}') AS d, NULLIF(doc, '{}') AS n FROM sd WHERE doc ? 'a' AND doc ? 'b' OFFSET 0) s;
-- and under a Sort the pointer, not the copy, is stored
SELECT a, b, pg_column_toast_chunk_id(d) IS NOT NULL AS pointer_kept
FROM (SELECT doc->'a' AS a, doc->'b' AS b, COALESCE(doc, '{}') AS d FROM sd ORDER BY small->>'a') s;
-- a holdable cursor is persisted through a receiver that detoasts anyway: the
-- scan still detoasts once while the cursor is materialized at COMMIT
BEGIN;
DECLARE hc CURSOR WITH HOLD FOR SELECT doc->'a', doc->'b' FROM sd;
COMMIT;
FETCH ALL FROM hc;
CLOSE hc;
-- a PL/pgSQL FOR loop fetches through SPI: one detoast per row
DO $$
DECLARE r record;
BEGIN
    FOR r IN SELECT doc->'a' AS a, doc->'b' AS b FROM sd LOOP
        RAISE NOTICE 'row: % %', r.a, r.b;
    END LOOP;
END $$;
-- shapes not covered above: several rows including a NULL, EXTENDED storage
-- (out of line and compressed), bytea, outer joins, other storing statements,
-- subscripting and jsonpath, kept aggregate arguments, window functions,
-- grouping sets, a scrollable cursor, set operations and a bitmap heap scan
CREATE TABLE sd3 (id int PRIMARY KEY, doc jsonb, blob bytea);
ALTER TABLE sd3 ALTER COLUMN doc SET STORAGE EXTENDED,
                ALTER COLUMN doc SET COMPRESSION pglz,
                ALTER COLUMN blob SET STORAGE EXTERNAL;
INSERT INTO sd3
SELECT i, CASE WHEN i = 2 THEN NULL ELSE
       ('{"a": ' || i || ', "b": 2}')::jsonb
         || (SELECT jsonb_object_agg('k' || j, repeat(md5((i * j)::text), 8)) FROM generate_series(1, 200) j) END,
       CASE WHEN i = 2 THEN NULL ELSE decode(repeat(md5(i::text), 600), 'hex') END
FROM generate_series(1, 3) i;
VACUUM ANALYZE sd3;
SELECT id, pg_column_compression(doc) AS compression, pg_column_toast_chunk_id(doc) IS NOT NULL AS out_of_line FROM sd3 ORDER BY id;
-- out of line and compressed (one fetch, decompressed by the same call), three
-- rows with one NULL: one detoast per non-null row
SELECT id, doc->'a', doc->'b', doc ? 'k1' FROM sd3 ORDER BY id;
-- bytea: two references, one detoast per row
SELECT id, length(blob) > 0, position('\x00'::bytea IN blob) FROM sd3 ORDER BY id;
-- subscripting and jsonpath count as detoasting references
SELECT doc['a'], doc['b'], jsonb_path_query_first(doc, '$.a'), doc @? '$.b' FROM sd3 WHERE id = 1;
-- left join: the null-extended side arrives as a null-filled slot
SET enable_nestloop = off; SET enable_mergejoin = off;
SELECT s.id, q.doc->'a', q.doc->'b' FROM sd s LEFT JOIN sd3 q ON q.id = s.id + 5 ORDER BY s.id;
SELECT s.id, q.doc->'a', q.doc->'b' FROM sd s LEFT JOIN sd3 q ON q.id = s.id ORDER BY s.id;
RESET enable_nestloop; RESET enable_mergejoin;
-- statements that store the column store the pointer while the WHERE
-- references share: one detoast per row for INSERT ... SELECT and for
-- CREATE TABLE AS
CREATE TABLE sd4 (LIKE sd3);
INSERT INTO sd4 SELECT id, doc, blob FROM sd3 WHERE doc ? 'a' AND doc ? 'b';
SELECT pg_column_toast_chunk_id(doc) IS NOT NULL AS pointer_kept, id FROM sd4 ORDER BY id;
CREATE TABLE sd5 AS SELECT id, doc FROM sd3 WHERE doc ? 'a' AND doc ? 'b';
SELECT pg_column_toast_chunk_id(doc) IS NOT NULL AS pointer_kept, id FROM sd5 ORDER BY id;
-- an aggregate that keeps its argument whole gets the stored pointer
SELECT jsonb_agg(doc ORDER BY id) IS NOT NULL FROM sd3 WHERE doc ? 'a' AND doc ? 'b';
-- a plain aggregate over several rows: the first row detoasts in the scan and
-- again from the Agg's copied tuple, the second row shares (three in all)
SELECT sum((doc->>'a')::int) FROM sd3 WHERE doc ? 'a';
-- window functions: the output expressions are evaluated by the WindowAgg on
-- rows read back from its tuplestore and share there; the scan's single
-- qual reference detoasts on its own (two detoasts per row)
SELECT doc->'a', doc->'b', count(*) OVER () FROM sd3 WHERE doc ? 'k1' ORDER BY 1;
-- grouping sets: the scan below shares like under any other parent
SELECT count(*) FROM sd3 WHERE doc ? 'a' AND doc ? 'b' GROUP BY GROUPING SETS ((id), ());
-- a scrollable cursor rescans and reads backward: one detoast per fetched row
BEGIN;
DECLARE sc SCROLL CURSOR FOR SELECT id, doc->'a', doc->'b' FROM sd3 ORDER BY id;
FETCH ALL FROM sc;
FETCH BACKWARD 2 FROM sc;
CLOSE sc;
COMMIT;
-- set operations: each input projects its expressions, so each scan shares
SELECT count(*) FROM (SELECT doc->'a', doc->'b' FROM sd3 WHERE doc ? 'a' UNION SELECT doc->'a', doc->'b' FROM sd3 WHERE doc ? 'b') u;
-- bitmap heap scan
CREATE INDEX sd3_docidx ON sd3 USING hash (id);
SET enable_seqscan = off; SET enable_indexscan = off;
SELECT doc->'a', doc->'b' FROM sd3 WHERE id = 1;
RESET enable_seqscan; RESET enable_indexscan;
DROP TABLE sd3, sd4, sd5;
-- an index-only scan reads the index tuple, where a large included value is
-- stored compressed: decompressed once
CREATE INDEX sd_ctxt_idx ON sd (id) INCLUDE (ctxt);
VACUUM sd;
SET enable_seqscan = off; SET enable_bitmapscan = off;
EXPLAIN (VERBOSE, COSTS OFF) SELECT length(md5(ctxt)), ctxt = ctxt FROM sd WHERE id = 1;
SELECT length(md5(ctxt)), ctxt = ctxt FROM sd WHERE id = 1;
RESET enable_seqscan; RESET enable_bitmapscan;
DROP INDEX sd_ctxt_idx;
-- an inline column never detoasts
SELECT small->'a', small->'b' FROM sd;
-- COPY compiles its WHERE clause against a ModifyTableState it builds
-- itself, which has no plan and no PlannedStmt to consult
CREATE TEMP TABLE sdcopy (id int, doc jsonb);
COPY sdcopy FROM stdin WHERE doc ? 'a' AND doc ? 'b';
1	{"a": 1, "b": 2}
2	{"c": 3}
\.
SELECT count(*) FROM sdcopy;
DROP TABLE sdcopy;
-- switching the feature off restores one detoast per reference and skips the
-- planning work
SET detoast_reuse = off;
SELECT doc->'a', doc->'b' FROM sd;
-- it covers the parameter path too, which no plan node's set can express:
-- two references inside a correlated subplan, so the copy is the parameter's
SELECT (SELECT (doc->>'a')::int + (doc->>'b')::int) FROM sd;
RESET detoast_reuse;
SELECT (SELECT (doc->>'a')::int + (doc->>'b')::int) FROM sd;

SELECT injection_points_detach('detoast-attr-external');
SELECT injection_points_detach('detoast-attr-compressed');
DROP TABLE sd, sdpar;

-- A slot refilled through a path other than ExecStore*/ExecClearTuple must
-- drop its copies too: a multi-batch hash join re-reads the probe side's
-- tuples from a batch file with ExecForceStoreMinimalTuple into a slot whose
-- previous copy would otherwise satisfy the hash clause for every following
-- tuple.  A CTE scan returns minimal tuples, which takes that path.
CREATE TABLE hb (id int, txt text);
ALTER TABLE hb ALTER COLUMN txt SET STORAGE EXTERNAL;
INSERT INTO hb SELECT i, 'v' || i || repeat(md5(i::text), 80) FROM generate_series(1, 4000) i;
CREATE TABLE hb2 (id int, txt text);
ALTER TABLE hb2 ALTER COLUMN txt SET STORAGE EXTERNAL;
INSERT INTO hb2 SELECT id, txt FROM hb WHERE id <= 3000;
VACUUM ANALYZE hb, hb2;
SET work_mem = '64kB'; SET enable_nestloop = off; SET enable_mergejoin = off;
EXPLAIN (VERBOSE, COSTS OFF) WITH o AS MATERIALIZED (SELECT txt FROM hb) SELECT count(*) FROM o JOIN hb2 i ON o.txt = i.txt;
WITH o AS MATERIALIZED (SELECT txt FROM hb) SELECT count(*) FROM o JOIN hb2 i ON o.txt = i.txt;
RESET work_mem; RESET enable_nestloop; RESET enable_mergejoin;
DROP TABLE hb, hb2;
