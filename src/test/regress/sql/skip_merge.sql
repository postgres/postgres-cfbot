--
-- Index Suffix Scan (MergeAppend)
--
-- Equality/IN on index prefix columns is expanded into per-prefix IndexPaths
-- merged with MergeAppend when ORDER BY starts with a later index column.
-- A qual on the suffix column is optional.  ASC/DESC and NULLS ordering must
-- match the index (or the opposite scan direction).
--

CREATE SCHEMA skip_merge;
SET search_path TO skip_merge;

CREATE TABLE test AS
SELECT x, y
FROM generate_series(1, 50) AS x,
     generate_series(1, 50) AS y;
CREATE INDEX test_idx ON test USING btree (x, y);
ANALYZE test;

CREATE TABLE t3 AS
SELECT a, b, c
FROM generate_series(1, 10) AS a,
     generate_series(1, 10) AS b,
     generate_series(1, 20) AS c;
CREATE INDEX t3_idx ON t3 USING btree (a, b, c);
ANALYZE t3;

CREATE TABLE tdesc AS
SELECT x, y
FROM generate_series(1, 20) AS x,
     generate_series(1, 20) AS y;
CREATE INDEX tdesc_idx ON tdesc USING btree (x, y DESC);
ANALYZE tdesc;

-- Nullable suffix for NULLS FIRST / LAST tests.
CREATE TABLE tnulls AS
SELECT x, y
FROM generate_series(1, 10) AS x,
     generate_series(1, 10) AS y
UNION ALL
SELECT x, NULL::int
FROM generate_series(1, 4) AS x;
CREATE INDEX tnulls_idx ON tnulls USING btree (x, y);			-- ASC, NULLS LAST
CREATE INDEX tnulls_nf_idx ON tnulls USING btree (x, y NULLS FIRST);
ANALYZE tnulls;

-- Same data as tnulls, but only a NULLS LAST index (mismatch cases).
CREATE TABLE tnulls_nl AS SELECT * FROM tnulls;
CREATE INDEX tnulls_nl_idx ON tnulls_nl USING btree (x, y);		-- ASC, NULLS LAST
ANALYZE tnulls_nl;

-- Same data as tnulls, but only a NULLS FIRST index (mismatch cases).
CREATE TABLE tnulls_nf AS SELECT * FROM tnulls;
CREATE INDEX tnulls_nf_only_idx ON tnulls_nf USING btree (x, y NULLS FIRST);
ANALYZE tnulls_nf;

-- Single-key btree index (unsupported: no suffix column).
CREATE TABLE tsingle AS SELECT * FROM test;
CREATE INDEX tsingle_idx ON tsingle USING btree (x);
ANALYZE tsingle;

-- Hash index (unsupported: no sortopfamily).
CREATE TABLE thash AS SELECT * FROM test;
CREATE INDEX thash_idx ON thash USING hash (x);
ANALYZE thash;

SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_indexskipmerge = on;

--
-- Supported: expect Merge Append
--

-- Basic: Const IN + ORDER BY suffix.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y LIMIT 3;

-- = ANY(array) form.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x = ANY (ARRAY[1, 2])
ORDER BY y LIMIT 3;

--
-- Ordered suffix with range filters
--

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y >= 10
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y > 10
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y < 10
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y <= 10
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y BETWEEN 10 AND 30
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) AND y >= 1 AND y <= 3
ORDER BY y LIMIT 3;

-- Secondary ORDER BY on prefix (MergeAppend compares full sort keys).
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y, x LIMIT 3;

-- At max_index_merge_scans limit.
SET max_index_merge_scans = 2;
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y LIMIT 3;
RESET max_index_merge_scans;

-- Multi-column prefix cartesian product.
EXPLAIN (COSTS OFF)
SELECT a, b, c FROM t3 WHERE a IN (1, 3) AND b IN (3, 4)
ORDER BY c LIMIT 3;

-- Equality OpExpr mixed with IN on another prefix column.
EXPLAIN (COSTS OFF)
SELECT a, b, c FROM t3 WHERE a = 1 AND b IN (3, 4)
ORDER BY c LIMIT 3;

-- Suffix is a middle index column.
EXPLAIN (COSTS OFF)
SELECT a, b, c
FROM t3 WHERE a IN (1, 3)
ORDER BY b LIMIT 3;

-- ORDER BY DESC on ASC index => Backward scan children.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3) ORDER BY y DESC
LIMIT 3;

-- DESC index + matching DESC order => Forward.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tdesc WHERE x IN (1, 3) ORDER BY y DESC
LIMIT 3;

-- DESC index + ASC order => Backward.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tdesc WHERE x IN (1, 3) ORDER BY y ASC
LIMIT 3;

-- Default ASC index NULLS LAST matches ORDER BY ... NULLS LAST.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tnulls WHERE x IN (1, 3) ORDER BY y NULLS LAST
LIMIT 3;

-- Explicit NULLS FIRST index matches ORDER BY ... NULLS FIRST.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tnulls WHERE x IN (1, 3) ORDER BY y NULLS FIRST
LIMIT 3;

-- No LIMIT (force Merge Append over SAOP+Sort).
SET enable_sort = off;
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y;
RESET enable_sort;

-- Outer Params (LATERAL) still accepted.
EXPLAIN (COSTS OFF)
SELECT s.*
FROM (VALUES (19), (20)) AS v(ymin),
LATERAL (
	SELECT x, y FROM test
	WHERE x IN (1, 3) AND y >= v.ymin
	ORDER BY y LIMIT 1
) s;

-- IN-list containing NULL expands (null-equality child included).
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x = ANY (ARRAY[1, NULL])
ORDER BY y
LIMIT 3;

--
-- Cases that do not choose Merge Append
--

-- Feature GUC off.
SET enable_indexskipmerge = off;
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y LIMIT 3;
RESET enable_indexskipmerge;

-- Indexscan GUC off.
SET enable_indexscan = off;
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY y LIMIT 3;
RESET enable_indexscan;

-- Fewer than two prefixes.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x = 1
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1)
ORDER BY y LIMIT 3;

-- No ORDER BY / ORDER BY leading column only.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 3)
ORDER BY x LIMIT 3;

-- Prefix range only (no equality/IN).
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x >= 1 AND x <= 2
ORDER BY y LIMIT 3;

-- Over max_index_merge_scans.
SET max_index_merge_scans = 2;
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (1, 2, 5)
ORDER BY y LIMIT 3;

-- Multi-prefix product over the GUC limit.
SET max_index_merge_scans = 3;
EXPLAIN (COSTS OFF)
SELECT a, b, c FROM t3 WHERE a IN (1, 3) AND b IN (3, 4)
ORDER BY c LIMIT 3;

-- Gap (b) between prefix constraints and sorting suffix
SET max_index_merge_scans = 3;
EXPLAIN (COSTS OFF)
SELECT a, b, c FROM t3 WHERE a IN (1, 3)
ORDER BY c LIMIT 3;


-- Non-Const / null / empty array SAOP.
EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x IN (SELECT u FROM generate_series(1, 2) u)
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x = ANY (NULL::int[])
ORDER BY y LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT x, y FROM test WHERE x = ANY ('{}'::int[])
ORDER BY y LIMIT 3;

-- Nulls order mismatch: only NULLS LAST index, query wants NULLS FIRST.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tnulls_nl WHERE x IN (1, 3) ORDER BY y NULLS FIRST
LIMIT 3;

-- Nulls order mismatch: only NULLS FIRST index, query wants NULLS LAST.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tnulls_nf
WHERE x IN (1, 3) ORDER BY y NULLS LAST
LIMIT 3;

-- Single-key index.
EXPLAIN (COSTS OFF)
SELECT x, y FROM tsingle
WHERE x IN (1, 3)
ORDER BY y LIMIT 3;

-- Hash index (no sortopfamily).
EXPLAIN (COSTS OFF)
SELECT x, y FROM thash
WHERE x IN (1, 3)
ORDER BY y LIMIT 3;

RESET ALL;
DROP SCHEMA skip_merge CASCADE;
