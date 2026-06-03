-- Cross-type operator support for int2/int4/int8 in GiST.
--
-- Verifies that (a) the cross-type B-tree-style operators registered in
-- gist_int{2,4,8}_ops match the results of seqscans using the same operator
-- expressions, (b) the KNN <-> operator works across types and uses the
-- index, and (c) values outside the smaller subtype's range are handled
-- according to normal comparison semantics, without narrowing or erroring.

--
-- The integer cross-type support is handled inside the existing
-- consistent/distance support functions (which dispatch on the subtype OID),
-- so the operator families must not contain any cross-type (different
-- left/right input type) support-function entries.
--
SELECT opf.opfname AS opfamily,
       amproc.amproclefttype::regtype AS lefttype,
       amproc.amprocrighttype::regtype AS righttype,
       amproc.amprocnum AS procnum,
       amproc.amproc::regproc AS proc
FROM pg_amproc amproc
     JOIN pg_opfamily opf ON opf.oid = amproc.amprocfamily
     JOIN pg_am am ON am.oid = opf.opfmethod
WHERE am.amname = 'gist'
  AND opf.opfname IN ('gist_int2_ops', 'gist_int4_ops', 'gist_int8_ops')
  AND amproc.amproclefttype <> amproc.amprocrighttype
ORDER BY opf.opfname,
         amproc.amproclefttype::regtype::text,
         amproc.amprocrighttype::regtype::text,
         amproc.amprocnum,
         amproc.amproc::regproc::text;

CREATE TABLE ct_i2 (a int2);
CREATE TABLE ct_i4 (a int4);
CREATE TABLE ct_i8 (a int8);

-- Use enough rows that each index has internal pages, so the internal-page
-- branches of the consistent and distance functions are exercised too, not
-- only the leaf-level ones. The run of duplicate 50s produces internal keys
-- whose lower and upper bounds are both 50, which the <> strategy can skip
-- without descending.
INSERT INTO ct_i2 SELECT g::int2 FROM generate_series(-1000, 1000) g;
INSERT INTO ct_i4 SELECT g FROM generate_series(-1000, 1000) g;
INSERT INTO ct_i8 SELECT g::int8 FROM generate_series(-1000, 1000) g;
INSERT INTO ct_i2 SELECT 50::int2 FROM generate_series(1, 1000);
INSERT INTO ct_i4 SELECT 50 FROM generate_series(1, 1000);
INSERT INTO ct_i8 SELECT 50::int8 FROM generate_series(1, 1000);

-- Add some values that are representable only in wider types, to exercise
-- the path where the cross-type query constant is out of range of the key
-- type.
INSERT INTO ct_i4 VALUES (100000), (-100000);
INSERT INTO ct_i8 VALUES (5000000000), (-5000000000);

CREATE INDEX ct_i2_idx ON ct_i2 USING gist (a);
CREATE INDEX ct_i4_idx ON ct_i4 USING gist (a);
CREATE INDEX ct_i8_idx ON ct_i8 USING gist (a);

ANALYZE ct_i2;
ANALYZE ct_i4;
ANALYZE ct_i8;

SET enable_seqscan = off;
SET enable_bitmapscan = off;

-- int2 key x int4 query
SELECT count(*) FROM ct_i2 WHERE a <  50::int4;
SELECT count(*) FROM ct_i2 WHERE a <= 50::int4;
SELECT count(*) FROM ct_i2 WHERE a =  50::int4;
SELECT count(*) FROM ct_i2 WHERE a >= 50::int4;
SELECT count(*) FROM ct_i2 WHERE a >  50::int4;
SELECT count(*) FROM ct_i2 WHERE a <> 50::int4;

-- query out of int2 range: matches nothing for =, everything for <>
SELECT count(*) FROM ct_i2 WHERE a =  100000::int4;
SELECT count(*) FROM ct_i2 WHERE a <> 100000::int4;
SELECT count(*) FROM ct_i2 WHERE a <  100000::int4;
SELECT count(*) FROM ct_i2 WHERE a >  100000::int4;

-- int2 key x int8 query
SELECT count(*) FROM ct_i2 WHERE a <  50::int8;
SELECT count(*) FROM ct_i2 WHERE a <= 50::int8;
SELECT count(*) FROM ct_i2 WHERE a =  50::int8;
SELECT count(*) FROM ct_i2 WHERE a >= 50::int8;
SELECT count(*) FROM ct_i2 WHERE a >  50::int8;
SELECT count(*) FROM ct_i2 WHERE a <> 50::int8;
SELECT count(*) FROM ct_i2 WHERE a =  5000000000::int8;
SELECT count(*) FROM ct_i2 WHERE a <  5000000000::int8;

-- int4 key x int2 query
SELECT count(*) FROM ct_i4 WHERE a <  50::int2;
SELECT count(*) FROM ct_i4 WHERE a <= 50::int2;
SELECT count(*) FROM ct_i4 WHERE a =  50::int2;
SELECT count(*) FROM ct_i4 WHERE a >= 50::int2;
SELECT count(*) FROM ct_i4 WHERE a >  50::int2;
SELECT count(*) FROM ct_i4 WHERE a <> 50::int2;

-- int4 key x int8 query
SELECT count(*) FROM ct_i4 WHERE a <  50::int8;
SELECT count(*) FROM ct_i4 WHERE a <= 50::int8;
SELECT count(*) FROM ct_i4 WHERE a =  50::int8;
SELECT count(*) FROM ct_i4 WHERE a >= 50::int8;
SELECT count(*) FROM ct_i4 WHERE a >  50::int8;
SELECT count(*) FROM ct_i4 WHERE a <> 50::int8;
SELECT count(*) FROM ct_i4 WHERE a =  5000000000::int8;
SELECT count(*) FROM ct_i4 WHERE a <  5000000000::int8;

-- int8 key x int2 query
SELECT count(*) FROM ct_i8 WHERE a <  50::int2;
SELECT count(*) FROM ct_i8 WHERE a <= 50::int2;
SELECT count(*) FROM ct_i8 WHERE a =  50::int2;
SELECT count(*) FROM ct_i8 WHERE a >= 50::int2;
SELECT count(*) FROM ct_i8 WHERE a >  50::int2;
SELECT count(*) FROM ct_i8 WHERE a <> 50::int2;

-- int8 key x int4 query
SELECT count(*) FROM ct_i8 WHERE a <  50::int4;
SELECT count(*) FROM ct_i8 WHERE a <= 50::int4;
SELECT count(*) FROM ct_i8 WHERE a =  50::int4;
SELECT count(*) FROM ct_i8 WHERE a >= 50::int4;
SELECT count(*) FROM ct_i8 WHERE a >  50::int4;
SELECT count(*) FROM ct_i8 WHERE a <> 50::int4;

--
-- The counts above would be the same if the planner fell back to a full index
-- scan that applies the predicate as a Filter, which disabling seqscan and
-- bitmapscan does not prevent. So check that each cross-type strategy becomes
-- an Index Cond, and that <-> for each type pair becomes an index Order By.
-- Only those plan lines (and any Filter) are shown, not the scan node, which
-- may be an Index Scan or an Index Only Scan depending on visibility-map
-- state. The list is spelled out rather than generated from pg_amop, so that
-- a missing pg_amop entry shows up here as a Filter.
--
CREATE FUNCTION ct_plan_quals(query text) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
    ln text;
    quals text[] := '{}';
BEGIN
    FOR ln IN EXECUTE 'EXPLAIN (COSTS OFF) ' || query
    LOOP
        IF ln ~ '(Index Cond|Order By|Filter): ' THEN
            quals := quals || btrim(ln);
        END IF;
    END LOOP;
    RETURN array_to_string(quals, '; ');
END
$$;

SELECT q AS query, ct_plan_quals(q) FROM (VALUES
    ('SELECT count(*) FROM ct_i2 WHERE a <  50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a <= 50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a =  50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a >= 50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a >  50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a <> 50::int4'),
    ('SELECT count(*) FROM ct_i2 WHERE a <  50::int8'),
    ('SELECT count(*) FROM ct_i2 WHERE a <= 50::int8'),
    ('SELECT count(*) FROM ct_i2 WHERE a =  50::int8'),
    ('SELECT count(*) FROM ct_i2 WHERE a >= 50::int8'),
    ('SELECT count(*) FROM ct_i2 WHERE a >  50::int8'),
    ('SELECT count(*) FROM ct_i2 WHERE a <> 50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a <  50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a <= 50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a =  50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a >= 50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a >  50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a <> 50::int2'),
    ('SELECT count(*) FROM ct_i4 WHERE a <  50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a <= 50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a =  50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a >= 50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a >  50::int8'),
    ('SELECT count(*) FROM ct_i4 WHERE a <> 50::int8'),
    ('SELECT count(*) FROM ct_i8 WHERE a <  50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a <= 50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a =  50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a >= 50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a >  50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a <> 50::int2'),
    ('SELECT count(*) FROM ct_i8 WHERE a <  50::int4'),
    ('SELECT count(*) FROM ct_i8 WHERE a <= 50::int4'),
    ('SELECT count(*) FROM ct_i8 WHERE a =  50::int4'),
    ('SELECT count(*) FROM ct_i8 WHERE a >= 50::int4'),
    ('SELECT count(*) FROM ct_i8 WHERE a >  50::int4'),
    ('SELECT count(*) FROM ct_i8 WHERE a <> 50::int4'),
    ('SELECT a FROM ct_i2 ORDER BY a <-> ''-100''::int4 LIMIT 3'),
    ('SELECT a FROM ct_i2 ORDER BY a <-> ''-100''::int8 LIMIT 3'),
    ('SELECT a FROM ct_i4 ORDER BY a <-> ''-100''::int2 LIMIT 3'),
    ('SELECT a FROM ct_i4 ORDER BY a <-> ''-100''::int8 LIMIT 3'),
    ('SELECT a FROM ct_i8 ORDER BY a <-> ''-100''::int2 LIMIT 3'),
    ('SELECT a FROM ct_i8 ORDER BY a <-> ''-100''::int4 LIMIT 3')
) v(q);

-- Confirm the index is actually used for a cross-type predicate.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM ct_i4 WHERE a = 50::int8;

-- Cross-type KNN. -100 lies in the middle of the data, so the scan computes
-- distances to internal keys on both sides of the query. -101 and -99 are
-- equidistant from it, so the three nearest rows are re-sorted by value to
-- keep the output stable.

-- Cross-type KNN: int4 key ordered by int2 / int8 queries.
EXPLAIN (COSTS OFF)
SELECT a FROM ct_i4 ORDER BY a <-> '-100'::int2 LIMIT 3;
SELECT a FROM (SELECT a FROM ct_i4 ORDER BY a <-> '-100'::int2 LIMIT 3) s ORDER BY a;

EXPLAIN (COSTS OFF)
SELECT a FROM ct_i4 ORDER BY a <-> '-100'::int8 LIMIT 3;
SELECT a FROM (SELECT a FROM ct_i4 ORDER BY a <-> '-100'::int8 LIMIT 3) s ORDER BY a;

-- Cross-type KNN: int2 key ordered by int4 / int8 queries.
SELECT a FROM (SELECT a FROM ct_i2 ORDER BY a <-> '-100'::int4 LIMIT 3) s ORDER BY a;
SELECT a FROM (SELECT a FROM ct_i2 ORDER BY a <-> '-100'::int8 LIMIT 3) s ORDER BY a;

-- Cross-type KNN: int8 key ordered by int2 / int4 queries.
SELECT a FROM (SELECT a FROM ct_i8 ORDER BY a <-> '-100'::int2 LIMIT 3) s ORDER BY a;
SELECT a FROM (SELECT a FROM ct_i8 ORDER BY a <-> '-100'::int4 LIMIT 3) s ORDER BY a;

-- Combined: cross-type WHERE + cross-type ORDER BY on the same index.
EXPLAIN (COSTS OFF)
SELECT a FROM ct_i4 WHERE a < 80::int8 ORDER BY a <-> '-100'::int8 LIMIT 3;
SELECT a FROM (SELECT a FROM ct_i4 WHERE a < 80::int8
               ORDER BY a <-> '-100'::int8 LIMIT 3) s ORDER BY a;

-- Standalone distance-function smoke tests (not going through the index),
-- including the overflow-detection paths.
SELECT int2_int4_dist(3::int2, 10::int4);
SELECT int4_int2_dist(-5::int4, 5::int2);
SELECT int2_int8_dist(3::int2, 10::int8);
SELECT int8_int2_dist(100::int8, -5::int2);
SELECT int4_int8_dist(100::int4, 5000000000::int8);
SELECT int8_int4_dist(5000000000::int8, 100::int4);

-- Overflow detection: INT32_MIN distance from a positive int2 can't fit
-- in int32, should error.
SELECT int2_int4_dist(1::int2, -2147483648::int4);
-- Likewise INT64_MIN distance from a positive int4 can't fit in int64.
SELECT int4_int8_dist(1::int4, -9223372036854775808::int8);

--
-- Multi-column GiST index with mixed-type predicates. This is the
-- original motivating case: without cross-type operator support the
-- planner can only use one column as an Index Cond and applies the
-- other(s) as a Filter post-scan. Here both columns should appear as
-- Index Cond.
--
CREATE TABLE ct_multi (a int4, b int8);
INSERT INTO ct_multi
    SELECT g, (g * 2)::int8 FROM generate_series(-50, 50) g;
CREATE INDEX ct_multi_idx ON ct_multi USING gist (a, b);
ANALYZE ct_multi;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM ct_multi WHERE a = 25::int8 AND b = 50::int4;

SELECT count(*) FROM ct_multi WHERE a = 25::int8 AND b = 50::int4;

-- Mixed cross-type ranges across both columns.
EXPLAIN (COSTS OFF)
SELECT count(*) FROM ct_multi WHERE a < 10::int8 AND b > 0::int2;

SELECT count(*) FROM ct_multi WHERE a < 10::int8 AND b > 0::int2;

DROP TABLE ct_multi;

--
-- Width-sensitivity guard (int16 vs int32).
--
-- The cross-type callbacks must read each side at its OWN width: the query at
-- the subtype width and the key at the indexed-column width, never narrowing
-- one to the other. These cases are constructed so that reading either side at
-- the wrong width changes the answer -- a dispatch entry returning the
-- wrong-width tinfo, an int4 key truncated to int16, or the query/key arguments
-- being swapped in gbt_num_consistent()/gbt_num_distance() would all flip the
-- result, because each wide constant shares its low 16 bits with a value present
-- in the data. enable_seqscan/bitmapscan are already off, and the plan checks
-- below confirm that each predicate is evaluated by the GiST consistent or
-- distance function (as an Index Cond or Order By) rather than as a Filter.
--
CREATE TABLE ct_w4 (k int4);
INSERT INTO ct_w4 VALUES (1), (65538);		-- 65538 = 0x10002, low 16 bits = 2
-- Non-positive filler rows give the index internal pages, so the internal
-- key bounds are read at the right width too; they don't change any of the
-- results below.
INSERT INTO ct_w4 SELECT g FROM generate_series(-2000, 0) g;
CREATE INDEX ct_w4_idx ON ct_w4 USING gist (k);
ANALYZE ct_w4;

-- int4 key vs int2 query: 65538 must compare as 65538, not as 2.
-- correct: {65538}; an int4 key truncated to int16 (65538 -> 2) would give {}.
SELECT k FROM ct_w4 WHERE k > 1000::int2 ORDER BY k;
-- correct: {} (no key equals 2); a truncated key (65538 -> 2) would match.
SELECT k FROM ct_w4 WHERE k = 2::int2 ORDER BY k;

-- KNN: nearest to 2 is 1 (distance 1). enable_sort=off steers the planner to
-- the GiST KNN distance path rather than a Sort on the operator. A truncated
-- key (65538 -> 2, distance 0) would wrongly sort 65538 first.
SET enable_sort = off;
SELECT k FROM ct_w4 ORDER BY k <-> 2::int2 LIMIT 1;
SELECT ct_plan_quals('SELECT k FROM ct_w4 ORDER BY k <-> 2::int2 LIMIT 1');
RESET enable_sort;

CREATE TABLE ct_w2 (k int2);
INSERT INTO ct_w2 VALUES (1), (50);
INSERT INTO ct_w2 SELECT g::int2 FROM generate_series(-2000, 0) g;	-- filler, as above
CREATE INDEX ct_w2_idx ON ct_w2 USING gist (k);
ANALYZE ct_w2;

-- int2 key vs int4 query: 65537 must compare as 65537, not as 1 (0x10001).
-- correct: 0; an int4 query truncated to int16 (65537 -> 1) would match k=1.
SELECT count(*) FROM ct_w2 WHERE k = 65537::int4;
-- correct: 0; a truncated query (65537 -> 1) would match k=50.
SELECT count(*) FROM ct_w2 WHERE k > 65537::int4;

SELECT q AS query, ct_plan_quals(q) FROM (VALUES
    ('SELECT k FROM ct_w4 WHERE k > 1000::int2 ORDER BY k'),
    ('SELECT k FROM ct_w4 WHERE k = 2::int2 ORDER BY k'),
    ('SELECT count(*) FROM ct_w2 WHERE k = 65537::int4'),
    ('SELECT count(*) FROM ct_w2 WHERE k > 65537::int4')
) v(q);

DROP TABLE ct_w4;
DROP TABLE ct_w2;

DROP TABLE ct_i2;
DROP TABLE ct_i4;
DROP TABLE ct_i8;
DROP FUNCTION ct_plan_quals(text);
