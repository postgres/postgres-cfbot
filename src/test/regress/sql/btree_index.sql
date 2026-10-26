--
-- BTREE_INDEX
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

CREATE TABLE bt_i4_heap (
	seqno 		int4,
	random 		int4
);

CREATE TABLE bt_name_heap (
	seqno 		name,
	random 		int4
);

CREATE TABLE bt_txt_heap (
	seqno 		text,
	random 		int4
);

CREATE TABLE bt_f8_heap (
	seqno 		float8,
	random 		int4
);

\set filename :abs_srcdir '/data/desc.data'
COPY bt_i4_heap FROM :'filename';

\set filename :abs_srcdir '/data/hash.data'
COPY bt_name_heap FROM :'filename';

\set filename :abs_srcdir '/data/desc.data'
COPY bt_txt_heap FROM :'filename';

\set filename :abs_srcdir '/data/hash.data'
COPY bt_f8_heap FROM :'filename';

ANALYZE bt_i4_heap;
ANALYZE bt_name_heap;
ANALYZE bt_txt_heap;
ANALYZE bt_f8_heap;

--
-- BTREE ascending/descending cases
--
-- we load int4/text from pure descending data (each key is a new
-- low key) and name/f8 from pure ascending data (each key is a new
-- high key).  we had a bug where new low keys would sometimes be
-- "lost".
--
CREATE INDEX bt_i4_index ON bt_i4_heap USING btree (seqno int4_ops);

CREATE INDEX bt_name_index ON bt_name_heap USING btree (seqno name_ops);

CREATE INDEX bt_txt_index ON bt_txt_heap USING btree (seqno text_ops);

CREATE INDEX bt_f8_index ON bt_f8_heap USING btree (seqno float8_ops);

--
-- test retrieval of min/max keys for each index
--

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno < 1;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno >= 9999;

SELECT b.*
   FROM bt_i4_heap b
   WHERE b.seqno = 4500;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno < '1'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno >= '9999'::name;

SELECT b.*
   FROM bt_name_heap b
   WHERE b.seqno = '4500'::name;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno < '1'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno >= '9999'::text;

SELECT b.*
   FROM bt_txt_heap b
   WHERE b.seqno = '4500'::text;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno < '1'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno >= '9999'::float8;

SELECT b.*
   FROM bt_f8_heap b
   WHERE b.seqno = '4500'::float8;

--
-- Add coverage of RowCompare quals whose row omits a column ("proargtypes")
-- that's after the first column, but before the final column.  The scan's
-- initial positioning strategy must become >= here (it's not the > strategy,
-- since the absence of "proargtypes" makes that tighter constraint unsafe).
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) > ('abs', 0)
ORDER BY proname, proargtypes, pronamespace LIMIT 1;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) > ('abs', 0)
ORDER BY proname, proargtypes, pronamespace LIMIT 1;

--
-- Similar to the previous test case, but this time it's a backwards scan
-- using a < RowCompare.  Must use the <= strategy (and not the < strategy).
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) < ('abs', 1_000_000)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC LIMIT 1;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, pronamespace) < ('abs', 1_000_000)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC LIMIT 1;

--
-- Forwards scan RowCompare qual whose row arg has a NULL that affects our
-- initial positioning strategy
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, proargtypes) >= ('abs', NULL) AND proname <= 'abs'
ORDER BY proname, proargtypes, pronamespace;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, proargtypes) >= ('abs', NULL) AND proname <= 'abs'
ORDER BY proname, proargtypes, pronamespace;

--
-- Forwards scan RowCompare quals whose row arg has a NULL that ends scan
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname >= 'abs' AND (proname, proargtypes) < ('abs', NULL)
ORDER BY proname, proargtypes, pronamespace;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname >= 'abs' AND (proname, proargtypes) < ('abs', NULL)
ORDER BY proname, proargtypes, pronamespace;

--
-- Backwards scan RowCompare qual whose row arg has a NULL that affects our
-- initial positioning strategy
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname >= 'abs' AND (proname, proargtypes) <= ('abs', NULL)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname >= 'abs' AND (proname, proargtypes) <= ('abs', NULL)
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

--
-- Backwards scan RowCompare qual whose row arg has a NULL that ends scan
--
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, proargtypes) > ('abs', NULL) AND proname <= 'abs'
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE (proname, proargtypes) > ('abs', NULL) AND proname <= 'abs'
ORDER BY proname DESC, proargtypes DESC, pronamespace DESC;

-- Makes B-Tree preprocessing deal with unmarking redundant keys that were
-- initially marked required (test case relies on current row compare
-- preprocessing limitations)
explain (costs off)
SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'zzzzzz' AND (proname, proargtypes) > ('abs', NULL)
   AND pronamespace IN (1, 2, 3) AND proargtypes IN ('26 23', '5077')
ORDER BY proname, proargtypes, pronamespace;

SELECT proname, proargtypes, pronamespace
   FROM pg_proc
   WHERE proname = 'zzzzzz' AND (proname, proargtypes) > ('abs', NULL)
   AND pronamespace IN (1, 2, 3) AND proargtypes IN ('26 23', '5077')
ORDER BY proname, proargtypes, pronamespace;

--
-- Test RowCompare handling within _bt_set_startikey, which decides whether
-- every tuple on a page (a page beyond the scan's first) must satisfy the
-- scan's RowCompare qual.
--
-- The index mixes an ASC column with a DESC column (so RowCompare members
-- don't all use the same inequality strategy and are not marked required),
-- uses a low fillfactor (so scans read several pages), and disables
-- deduplication (so the "b" NULLs span more than one page).
create temp table btree_rowcompare_tab (a int, b int, c int);
insert into btree_rowcompare_tab
  select a, b, b from generate_series(1, 3) a, generate_series(1, 150) b;
insert into btree_rowcompare_tab
  select 2, null, null from generate_series(1, 50);
create index btree_rowcompare_idx on btree_rowcompare_tab (a, b desc, c)
  with (fillfactor = 10, deduplicate_items = off);
vacuum analyze btree_rowcompare_tab;

set enable_seqscan to false;
set enable_bitmapscan to false;

-- RowCompare satisfied by every tuple on many pages (decided by its first
-- member on "a = 3" pages, and by its final member on "a = 2" pages)
explain (costs off)
select count(*) from btree_rowcompare_tab where (a, b) >= (2, 75);
select count(*) from btree_rowcompare_tab where (a, b) >= (2, 75);

-- Reaches the RowCompare's unsatisfiable NULL member argument on "a = 2"
-- pages (the "a = 2" key positions the scan within the "a = 2" group, which
-- the RowCompare qual alone would not).  The combined quals are
-- contradictory, but preprocessing cannot detect that.
explain (costs off)
select count(*) from btree_rowcompare_tab where a = 2 and (a, b) >= (2, null);
select count(*) from btree_rowcompare_tab where a = 2 and (a, b) >= (2, null);

-- RowCompare's row omits the index's second column, so on pages whose "b"
-- values change _bt_set_startikey can't prove that every tuple satisfies the
-- RowCompare.
explain (costs off)
select count(*) from btree_rowcompare_tab where (a, c) >= (2, 100);
select count(*) from btree_rowcompare_tab where (a, c) >= (2, 100);

-- Variant that uses the remaining inequality strategies
explain (costs off)
select count(*) from btree_rowcompare_tab where (a, b) < (2, 10);
select count(*) from btree_rowcompare_tab where (a, b) < (2, 10);

drop table btree_rowcompare_tab;

--
-- Test SAOP array handling within _bt_set_startikey
--
create temp table btree_saop_tab (a int, b int, c int);
insert into btree_saop_tab
  select a, b, b from generate_series(1, 3) a, generate_series(1, 150) b;
insert into btree_saop_tab
  select 2, 0, 7 from generate_series(1, 60);
create index btree_saop_idx on btree_saop_tab (a, b desc, c)
  with (fillfactor = 10, deduplicate_items = off);
vacuum analyze btree_saop_tab;

-- SAOP on the leading column: pages beyond each primitive scan's first page
-- have a single "a" value that a binary search finds in the array, so the
-- scan starts past the SAOP key (forcing the nonrequired key protocol)
explain (costs off)
select count(*) from btree_saop_tab where a in (1, 3) and b >= 100;
select count(*) from btree_saop_tab where a in (1, 3) and b >= 100;

-- Skip array on "b" precedes the "c" SAOP; pages whose "b" values change
-- prevent starting past the "c" SAOP key
explain (costs off)
select count(*) from btree_saop_tab where a = 2 and c in (101, 105);
select count(*) from btree_saop_tab where a = 2 and c in (101, 105);

-- "c" SAOP follows the "b" inequality; on pages that lie wholly within the
-- duplicate "(2, 0, 7)" run, the scan starts past all of its scan keys,
-- including the SAOP key
explain (costs off)
select count(*) from btree_saop_tab where a = 2 and b < 1 and c in (6, 7);
select count(*) from btree_saop_tab where a = 2 and b < 1 and c in (6, 7);

reset enable_seqscan;
reset enable_bitmapscan;
drop table btree_saop_tab;

--
-- Performs a recheck of > key following array advancement on previous (left
-- sibling) page that used a high key whose attribute value corresponding to
-- the > key was -inf (due to being truncated when the high key was created).
--
-- XXX This relies on the assumption that tenk1_thous_tenthous has a truncated
-- high key "(183, -inf)" on the first page that we'll scan.  The test will only
-- provide useful coverage when the default 8K BLCKSZ is in use.
--
explain (costs off)
SELECT thousand, tenthous
  FROM tenk1
  WHERE thousand IN (182, 183) AND tenthous > 7550;

SELECT thousand, tenthous
  FROM tenk1
  WHERE thousand IN (182, 183) AND tenthous > 7550;

--
-- Add coverage for optimization of backwards scan index descents
--
-- Here we expect _bt_search to descend straight to a leaf page containing a
-- non-pivot tuple with the value '47', which comes last (after 11 similar
-- non-pivot tuples).  Query execution should only need to visit a single
-- leaf page here.
--
-- Test case relies on tenk1_hundred index having a leaf page whose high key
-- is '(48, -inf)'.  We use a low cardinality index to make our test case less
-- sensitive to implementation details that may change in the future.
set enable_seqscan to false;
set enable_indexscan to true;
set enable_bitmapscan to false;
explain (costs off)
select hundred, twenty from tenk1 where hundred < 48 order by hundred desc limit 1;
select hundred, twenty from tenk1 where hundred < 48 order by hundred desc limit 1;

-- This variant of the query need only return a single tuple located to the immediate
-- right of the '(48, -inf)' high key.  It also only needs to scan one single
-- leaf page (the right sibling of the page scanned by the last test case):
explain (costs off)
select hundred, twenty from tenk1 where hundred <= 48 order by hundred desc limit 1;
select hundred, twenty from tenk1 where hundred <= 48 order by hundred desc limit 1;

--
-- Add coverage for ScalarArrayOp btree quals with pivot tuple constants
--
explain (costs off)
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82);
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82);

explain (costs off)
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82) order by hundred desc;
select distinct hundred from tenk1 where hundred in (47, 48, 72, 82) order by hundred desc;

explain (costs off)
select thousand from tenk1 where thousand in (364, 366,380) and tenthous = 200000;
select thousand from tenk1 where thousand in (364, 366,380) and tenthous = 200000;

--
-- Check correct optimization of LIKE (special index operator support)
-- for both indexscan and bitmapscan cases
--

set enable_seqscan to false;
set enable_indexscan to true;
set enable_bitmapscan to false;
explain (costs off)
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike '00%foo' order by 1;
select proname from pg_proc where proname ilike '00%foo' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike 'ri%foo' order by 1;

set enable_indexscan to false;
set enable_bitmapscan to true;
explain (costs off)
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
select proname from pg_proc where proname like E'RI\\_FKey%del' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike '00%foo' order by 1;
select proname from pg_proc where proname ilike '00%foo' order by 1;
explain (costs off)
select proname from pg_proc where proname ilike 'ri%foo' order by 1;

reset enable_seqscan;
reset enable_indexscan;
reset enable_bitmapscan;

-- Also check LIKE optimization with binary-compatible cases

create temp table btree_bpchar (f1 text collate "C");
create index on btree_bpchar(f1 bpchar_ops) WITH (deduplicate_items=on);
insert into btree_bpchar values ('foo'), ('fool'), ('bar'), ('quux');
-- doesn't match index:
explain (costs off)
select * from btree_bpchar where f1 like 'foo';
select * from btree_bpchar where f1 like 'foo';
explain (costs off)
select * from btree_bpchar where f1 like 'foo%';
select * from btree_bpchar where f1 like 'foo%';
-- these do match the index:
explain (costs off)
select * from btree_bpchar where f1::bpchar like 'foo';
select * from btree_bpchar where f1::bpchar like 'foo';
explain (costs off)
select * from btree_bpchar where f1::bpchar like 'foo%';
select * from btree_bpchar where f1::bpchar like 'foo%';

--
-- Perform unique checking, with and without the use of deduplication
--
CREATE TABLE dedup_unique_test_table (a int) WITH (autovacuum_enabled=false);
CREATE UNIQUE INDEX dedup_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=on);
CREATE UNIQUE INDEX plain_unique ON dedup_unique_test_table (a) WITH (deduplicate_items=off);
-- Generate enough garbage tuples in index to ensure that even the unique index
-- with deduplication enabled has to check multiple leaf pages during unique
-- checking (at least with a BLCKSZ of 8192 or less)
DO $$
BEGIN
    FOR r IN 1..1350 LOOP
        DELETE FROM dedup_unique_test_table;
        INSERT INTO dedup_unique_test_table SELECT 1;
    END LOOP;
END$$;

-- Exercise the LP_DEAD-bit-set tuple deletion code with a posting list tuple.
-- The implementation prefers deleting existing items to merging any duplicate
-- tuples into a posting list, so we need an explicit test to make sure we get
-- coverage (note that this test also assumes BLCKSZ is 8192 or less):
DROP INDEX plain_unique;
DELETE FROM dedup_unique_test_table WHERE a = 1;
INSERT INTO dedup_unique_test_table SELECT i FROM generate_series(0,450) i;

--
-- Test B-tree fast path (cache rightmost leaf page) optimization.
--

-- First create a tree that's at least three levels deep (i.e. has one level
-- between the root and leaf levels). The text inserted is long.  It won't be
-- TOAST compressed because we use plain storage in the table.  Only a few
-- index tuples fit on each internal page, allowing us to get a tall tree with
-- few pages.  (A tall tree is required to trigger caching.)
--
-- The text column must be the leading column in the index, since suffix
-- truncation would otherwise truncate tuples on internal pages, leaving us
-- with a short tree.
create table btree_tall_tbl(id int4, t text);
alter table btree_tall_tbl alter COLUMN t set storage plain;
create index btree_tall_idx on btree_tall_tbl (t, id) with (fillfactor = 10);
insert into btree_tall_tbl select g, repeat('x', 250)
from generate_series(1, 130) g;
insert into btree_tall_tbl select g, NULL
from generate_series(50, 60) g;

--
-- Test for skip scan with type that lacks skip support (text)
--
set enable_seqscan to false;
set enable_bitmapscan to false;

-- Forwards scan
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t, id;
explain (costs off)
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t, id;

-- Backwards scan
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t DESC, id DESC;
explain (costs off)
SELECT id FROM btree_tall_tbl WHERE id = 55 ORDER BY t DESC, id DESC;

reset enable_seqscan;
reset enable_bitmapscan;

--
-- Test for multilevel page deletion
--
CREATE TABLE delete_test_table (a bigint, b bigint, c bigint, d bigint);
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,80000) i;
ALTER TABLE delete_test_table ADD PRIMARY KEY (a,b,c,d);
-- Delete most entries, and vacuum, deleting internal pages and creating "fast
-- root"
DELETE FROM delete_test_table WHERE a < 79990;
VACUUM delete_test_table;

--
-- Test B-tree insertion with a metapage update (XLOG_BTREE_INSERT_META
-- WAL record type). This happens when a "fast root" page is split.  This
-- also creates coverage for nbtree FSM page recycling.
--
-- The vacuum above should've turned the leaf page into a fast root. We just
-- need to insert some rows to cause the fast root page to split.
INSERT INTO delete_test_table SELECT i, 1, 2, 3 FROM generate_series(1,1000) i;

-- Test unsupported btree opclass parameters
create index on btree_tall_tbl (id int4_ops(foo=1));

-- test parallel build with immutable function.
CREATE TABLE btree_test_expr (n int);
CREATE FUNCTION btree_test_func() RETURNS int LANGUAGE sql IMMUTABLE RETURN 0;
BEGIN;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL max_parallel_maintenance_workers = 4;
CREATE INDEX btree_test_expr_idx ON btree_test_expr USING btree (btree_test_func());
COMMIT;
DROP TABLE btree_test_expr;
DROP FUNCTION btree_test_func();

-- Test case of ALTER INDEX with abuse of column names for indexes.
-- This grammar is not officially supported, but the parser allows it.
CREATE INDEX btree_tall_idx2 ON btree_tall_tbl (id);
ALTER INDEX btree_tall_idx2 ALTER COLUMN id SET (n_distinct=100);
DROP INDEX btree_tall_idx2;
-- Partitioned index
CREATE TABLE btree_part (id int4) PARTITION BY RANGE (id);
CREATE INDEX btree_part_idx ON btree_part(id);
ALTER INDEX btree_part_idx ALTER COLUMN id SET (n_distinct=100);
DROP TABLE btree_part;

--
-- Leaf key prefix compression (prefix_compression reloption).
--
-- Correctness is checked against an independent oracle: a forced index scan
-- over a prefix-compressed index must return exactly the rows a sequential
-- scan of the same data returns, across build, retail insert/split, delete
-- and VACUUM, for equality, range, backward, index-only and NULL queries.
-- The key column uses the C collation so the recorded output is deterministic
-- across platforms.  The page format is exercised by keys that share a long
-- prefix (URL-like), by keys sharing exactly 125/126/127 bytes (the boundary
-- where the one-byte shared count saturates: BTREE_PREFIX_MAX is 126,
-- BTREE_PREFIX_RAW is 127), by keys that share nothing, and by a NULL key.
--
CREATE TABLE btree_prefix (u text COLLATE "C");
-- Build-path pages (CREATE INDEX writes whole leaves).  A shared 40-byte
-- prefix, plus explicit rows at the 125/126/127 shared-byte boundary.
INSERT INTO btree_prefix
  SELECT 'https://example.com/path/to/resource/' || lpad((g % 20000)::text, 8, '0')
  FROM generate_series(1, 20000) g;
INSERT INTO btree_prefix
  SELECT repeat('a', 40) || repeat('b', n) FROM generate_series(85, 90) n;
INSERT INTO btree_prefix VALUES
  (repeat('a', 125)), (repeat('a', 126)), (repeat('a', 127)), (repeat('a', 300)),
  ('zzz-no-shared-prefix'), (NULL);
CREATE INDEX btree_prefix_on ON btree_prefix (u) WITH (prefix_compression = on);

-- Confirm the index really is used (and is prefix-compressed), so the queries
-- below exercise the coded page format rather than silently falling back.
SET enable_seqscan = off;
SET enable_bitmapscan = off;
EXPLAIN (costs off)
  SELECT u FROM btree_prefix WHERE u = 'https://example.com/path/to/resource/00012345';

-- Equality, range and backward scans forced through the prefix index.  Their
-- results are recorded in the expected output, which is itself the oracle: a
-- decode regression changes these rows.  (The index-only digest below adds an
-- oracle that does not read the index at all.)
SELECT count(*) FROM btree_prefix
  WHERE u = 'https://example.com/path/to/resource/00012345';
SELECT count(*) FROM btree_prefix
  WHERE u BETWEEN 'https://example.com/path/to/resource/00000100'
             AND 'https://example.com/path/to/resource/00000900';
SELECT u FROM btree_prefix
  WHERE u > 'https://example.com/path/to/resource/00019996' ORDER BY u DESC;
-- Boundary and oversized keys round-trip to their exact stored values.
SELECT u FROM btree_prefix WHERE u LIKE 'aaaa%' ORDER BY u;
SELECT count(*) FROM btree_prefix WHERE u = repeat('a', 300);
SELECT count(*) FROM btree_prefix WHERE u IS NULL;

-- Index-only scan over the prefix index, compared to the literal values that
-- were inserted (an oracle that does not itself read the index).
SELECT (SELECT md5(string_agg(u, '|' ORDER BY u))
          FROM btree_prefix WHERE u IS NOT NULL) =
       md5(string_agg(x, '|' ORDER BY x)) AS ios_ok
  FROM (
    SELECT 'https://example.com/path/to/resource/' || lpad((g % 20000)::text, 8, '0') AS x
      FROM generate_series(1, 20000) g
    UNION ALL SELECT repeat('a', 40) || repeat('b', n) FROM generate_series(85, 90) n
    UNION ALL SELECT v FROM (VALUES (repeat('a',125)),(repeat('a',126)),
                                    (repeat('a',127)),(repeat('a',300)),
                                    ('zzz-no-shared-prefix')) t(v)
  ) oracle;

-- Retail inserts cause splits that fault pages forward into the coded format;
-- an index-only scan must still agree with the independent oracle afterwards.
INSERT INTO btree_prefix
  SELECT 'https://example.com/other/' || lpad(g::text, 8, '0')
  FROM generate_series(1, 10000) g;
-- Delete then VACUUM, then re-insert into freed space.  The delete targets
-- only part of the URL namespace so the boundary keys (aaaa.../zzz...) survive
-- for the checks below.
DELETE FROM btree_prefix
  WHERE u >= 'https://example.com/path/to/resource/00000000'
    AND u <  'https://example.com/path/to/resource/00005000';
VACUUM btree_prefix;
INSERT INTO btree_prefix
  SELECT 'https://example.com/path/to/resource/' || lpad((g % 5000)::text, 8, '0')
  FROM generate_series(1, 5000) g;
-- After the churn, an index-only scan of the live keys must equal the same
-- keys fetched from the heap by a sequential scan (an independent path): if
-- decoding the coded leaves were wrong, the two digests would differ.
SET enable_indexonlyscan = on;
SELECT md5(string_agg(u, '|' ORDER BY u)) AS ios_digest
  FROM btree_prefix WHERE u IS NOT NULL \gset
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_seqscan = on;
SELECT :'ios_digest' =
       (SELECT md5(string_agg(u, '|' ORDER BY u)) FROM btree_prefix WHERE u IS NOT NULL)
       AS post_churn_ok;
SET enable_indexscan = on;
RESET enable_seqscan;
RESET enable_bitmapscan;

-- The reloption round-trips and can be toggled without a rewrite.
SELECT reloptions FROM pg_class WHERE relname = 'btree_prefix_on';
ALTER INDEX btree_prefix_on SET (prefix_compression = off);
SELECT reloptions FROM pg_class WHERE relname = 'btree_prefix_on';
-- Reading back with the option off still returns correct rows: existing
-- BTP_PREFIX leaves keep their coded form and decode on the page flag.
SET enable_seqscan = off;
SELECT count(*) FROM btree_prefix WHERE u = repeat('a', 126);
RESET enable_seqscan;

-- A non-varlena first column is legal and silently stores no prefix; verify a
-- scan over such an index returns correct rows.
CREATE INDEX btree_prefix_int ON btree_prefix ((length(u))) WITH (prefix_compression = on);
SET enable_seqscan = off;
SELECT length(u) FROM btree_prefix WHERE length(u) = 300;
RESET enable_seqscan;
DROP TABLE btree_prefix;

--
-- Bottom-up index deletion on prefix-compressed pages.
--
-- An UPDATE that does not change the indexed column marks the resulting index
-- insert "logically unchanged", which lets nbtree run a bottom-up deletion
-- pass before splitting a leaf.  That pass reads and (only) removes items; it
-- must work on coded pages too.  Here we exercise the path and verify the
-- index stays correct; committed update batches between VACUUMs leave
-- removable duplicates for the pass to find.  (Bloat is quantified in the
-- performance benchmark, not here, since removability is timing-dependent.)
--
CREATE TABLE btree_prefix_bu (id int PRIMARY KEY, u text COLLATE "C", filler text);
INSERT INTO btree_prefix_bu
  SELECT g, 'https://example.com/path/' || lpad((g % 500)::text, 8, '0'), repeat('x', 40)
  FROM generate_series(1, 4000) g;
CREATE INDEX btree_prefix_bu_u ON btree_prefix_bu (u) WITH (prefix_compression = on);
-- Several committed non-key UPDATE batches (version churn on the prefix index).
UPDATE btree_prefix_bu SET filler = repeat('y', 40);
UPDATE btree_prefix_bu SET filler = repeat('z', 40);
UPDATE btree_prefix_bu SET filler = repeat('w', 40);
-- Correctness after the churn: an index-only scan of the key column must equal
-- the independent oracle (the literal keys), proving no decode/deletion damage.
SET enable_seqscan = off;
SELECT (SELECT md5(string_agg(u, '|' ORDER BY u)) FROM btree_prefix_bu) =
       md5(string_agg(x, '|' ORDER BY x)) AS bottomup_ok
  FROM (SELECT 'https://example.com/path/' || lpad((g % 500)::text, 8, '0') AS x
          FROM generate_series(1, 4000) g) oracle;
SELECT count(*) FROM btree_prefix_bu
  WHERE u = 'https://example.com/path/' || lpad('7', 8, '0');
RESET enable_seqscan;
DROP TABLE btree_prefix_bu;

--
-- Runtime deduplication on a prefix-compressed page.  Duplicate keys inserted
-- one at a time into an existing prefix index fill a leaf; before it splits,
-- the deduplication pass decodes the coded items, merges the duplicate TIDs
-- into posting lists and re-codes the page.  The decoded set can exceed one
-- page even when the coded form fit, in which case the pass aborts and the
-- page splits instead; either way the result must be correct.
--
CREATE TABLE btree_prefix_dd (u text COLLATE "C");
CREATE INDEX btree_prefix_dd_u ON btree_prefix_dd (u) WITH (prefix_compression = on);
INSERT INTO btree_prefix_dd
  SELECT 'https://example.com/path/to/resource/' || lpad((g % 200)::text, 8, '0')
  FROM generate_series(1, 40000) g;
SET enable_seqscan = off;
SELECT (SELECT md5(string_agg(u, '|' ORDER BY u)) FROM btree_prefix_dd) =
       md5(string_agg(x, '|' ORDER BY x)) AS runtime_dedup_ok
  FROM (SELECT 'https://example.com/path/to/resource/' || lpad((g % 200)::text, 8, '0') AS x
          FROM generate_series(1, 40000) g) oracle;
SELECT count(*) FROM btree_prefix_dd
  WHERE u = 'https://example.com/path/to/resource/' || lpad('42', 8, '0');
RESET enable_seqscan;
DROP TABLE btree_prefix_dd;
