-- Join MCV statistics tests
--
-- Note: tables for which we check estimated row counts should be created
-- with autovacuum_enabled = off, so that we don't have unstable results
-- from auto-analyze happening when we didn't expect it.
--

--
-- Verify CREATE STATISTICS rejects invalid join syntax.
--

-- Minimal tables for error-case testing.
CREATE TABLE dim_t1 (id INTEGER PRIMARY KEY, val TEXT NOT NULL);
CREATE TABLE fact (id INTEGER PRIMARY KEY, dim_t1_id INTEGER NOT NULL);
INSERT INTO dim_t1 VALUES (1, 'x');
INSERT INTO fact VALUES (1, 1);
ANALYZE dim_t1;
ANALYZE fact;

CREATE STATISTICS bad_stats1 (mcv) ON dim_t1.val;
CREATE STATISTICS bad_stats2 (mcv) ON dim_t1.val FROM fact, dim_t1;
CREATE STATISTICS bad_stats3 (mcv) FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS bad_stats4 (mcv) ON val FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS bad_stats5 (mcv) ON lower(dim_t1.val) FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
-- Composite join condition (AND) is not supported
CREATE STATISTICS bad_composite (mcv) ON dim_t1.val
FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id AND fact.id = dim_t1.id);
-- Non-Var operand in join condition (constant)
CREATE STATISTICS bad_const_join (mcv) ON dim_t1.val
FROM fact JOIN dim_t1 ON (dim_t1.id = 1);
-- Non-equality join condition
CREATE STATISTICS bad_non_eq (mcv) ON dim_t1.val
FROM fact JOIN dim_t1 ON (fact.dim_t1_id > dim_t1.id);
-- Non-inner join types are not supported
CREATE STATISTICS bad_left (mcv) ON dim_t1.val
FROM fact LEFT JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS bad_right (mcv) ON dim_t1.val
FROM fact RIGHT JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS bad_full (mcv) ON dim_t1.val
FROM fact FULL JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS bad_cross (mcv) ON dim_t1.val
FROM fact CROSS JOIN dim_t1;
-- Whole-row reference in join condition is not supported
CREATE STATISTICS bad_wholerow (mcv) ON dim_t1.val
FROM fact JOIN dim_t1 ON (fact = dim_t1);
-- Subquery in FROM clause is not supported
CREATE STATISTICS bad_stats6 (mcv) ON x.val FROM (SELECT * FROM dim_t1) x JOIN fact ON (x.id = fact.dim_t1_id);
-- Self-join: both aliases refer to the same table.
-- CREATE succeeds; the stat is valid but rarely useful in practice.
CREATE STATISTICS self_join_stat (mcv) ON b.val
FROM dim_t1 a JOIN dim_t1 b ON (a.id = b.id);
DROP STATISTICS self_join_stat;
-- Virtual generated column as a join stat target is not supported.
CREATE TABLE vgc_probe (id INTEGER PRIMARY KEY, v INTEGER,
                        g INTEGER GENERATED ALWAYS AS (v * 2) VIRTUAL);
CREATE STATISTICS bad_vgc (mcv) ON vgc_probe.g
FROM fact JOIN vgc_probe ON (fact.dim_t1_id = vgc_probe.id);
DROP TABLE vgc_probe;
-- No suitable index on join column
CREATE TABLE no_idx (id INTEGER, val TEXT);
INSERT INTO no_idx VALUES (1, 'x');
CREATE STATISTICS bad_stats7 (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
-- BRIN index exists but doesn't support equality lookup
CREATE INDEX no_idx_brin ON no_idx USING brin (id);
CREATE STATISTICS bad_stats8 (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
DROP INDEX no_idx_brin;
-- Partial index excluded (only indexes a subset of rows)
CREATE INDEX no_idx_partial ON no_idx (id) WHERE id > 0;
CREATE STATISTICS bad_stats_partial (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
DROP INDEX no_idx_partial CASCADE;
-- Btree index supports equality lookup, succeeds
CREATE INDEX ON no_idx (id);
CREATE STATISTICS idx_dep_stats (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
-- DROP INDEX refuses due to dependency
DROP INDEX no_idx_id_idx;
-- DROP INDEX CASCADE drops both the index and the dependent stats
DROP INDEX no_idx_id_idx CASCADE;
SELECT count(*) FROM pg_statistic_ext WHERE stxname = 'idx_dep_stats';
-- Multi-column index: usable only when the join column is the leading key column.
CREATE INDEX no_idx_lead ON no_idx (id, val);
CREATE STATISTICS idx_lead_ok (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
DROP STATISTICS idx_lead_ok;
DROP INDEX no_idx_lead;
-- Join column not the leading key column: CREATE fails.
CREATE INDEX no_idx_nonlead ON no_idx (val, id);
CREATE STATISTICS bad_idx_nonlead (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
DROP INDEX no_idx_nonlead;
-- A more optimal index created after the statistics object: the dependency stays
-- on the index chosen at CREATE, while ANALYZE uses the better index for cheaper
-- sampling.  The later index is not depended on, so it drops freely; the recorded
-- index stays protected even though an equivalent index now exists.
CREATE INDEX no_idx_wide ON no_idx (id, val);
CREATE STATISTICS idx_equiv (mcv) ON no_idx.val
FROM fact JOIN no_idx ON (fact.dim_t1_id = no_idx.id);
CREATE INDEX no_idx_narrow ON no_idx (id);
ANALYZE fact;
-- Not depended on: drops freely
DROP INDEX no_idx_narrow;
-- Depended on: drop blocked
DROP INDEX no_idx_wide;
DROP STATISTICS idx_equiv;
DROP INDEX no_idx_wide;
DROP TABLE no_idx;

-- Join stats on a type without less-than operator should fail (even single-column,
-- because the MCV builder always needs a sort operator)
CREATE TABLE xid_tab (id INTEGER PRIMARY KEY, w xid);
CREATE INDEX ON xid_tab (id);
INSERT INTO xid_tab VALUES (1, '1');
ANALYZE xid_tab;
CREATE STATISTICS jstat_xid (mcv) ON xid_tab.w
FROM fact JOIN xid_tab ON (fact.dim_t1_id = xid_tab.id);
CREATE STATISTICS jstat_xid_multi (mcv) ON xid_tab.w, fact.dim_t1_id
FROM fact JOIN xid_tab ON (fact.dim_t1_id = xid_tab.id);
DROP TABLE xid_tab;

-- 3-way join: CREATE STATISTICS succeeds, ANALYZE warns that collection
-- is not yet implemented for n-way joins.
CREATE TABLE dim_t2 (id INTEGER PRIMARY KEY, label TEXT NOT NULL);
INSERT INTO dim_t2 VALUES (1, 'a');
ANALYZE dim_t2;

-- 3-way join with filter columns from two different join relations.
-- Verify keyrefs maps each column to its correct source relation.
CREATE STATISTICS nway_multi_filter (mcv) ON dim_t1.val, dim_t2.label
    FROM fact
    JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id)
    JOIN dim_t2 ON (dim_t1.id = dim_t2.id);

-- keyrefs should be {2, 3}: val from varno 2=dim_t1,
-- label from varno 3=dim_t2
SELECT s.stxname,
       s.stxrelid::regclass,
       s.stxjoinrels::regclass[],
       s.stxkind
FROM pg_statistic_ext s
WHERE s.stxname = 'nway_multi_filter';

SELECT pg_get_statisticsobjdef_columns_from(oid) FROM pg_statistic_ext WHERE stxname = 'nway_multi_filter';

-- ANALYZE warns that n-way collection is not yet supported
ANALYZE fact;

DROP STATISTICS nway_multi_filter;

-- 3-way join with single filter: also warns
CREATE STATISTICS nway_stats (mcv) ON dim_t1.val
    FROM fact
    JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id)
    JOIN dim_t2 ON (dim_t1.id = dim_t2.id);

ANALYZE fact;

DROP STATISTICS nway_stats;

-- Ensure statistics are dropped when target columns are
CREATE STATISTICS dep_jstat_val (mcv) ON dim_t1.val
    FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
CREATE STATISTICS dep_jstat_t1id (mcv) ON fact.dim_t1_id
    FROM fact JOIN dim_t1 ON (fact.dim_t1_id = dim_t1.id);
ALTER TABLE dim_t1 DROP COLUMN val;
\d fact
ALTER TABLE fact DROP COLUMN dim_t1_id;
\d fact

DROP TABLE dim_t1, fact, dim_t2;

--
-- Test join MCV statistics creation, collection and usage.
--

CREATE TABLE keywords (
    id INTEGER PRIMARY KEY,
    keyword TEXT NOT NULL,
    phonetic_code character varying(5)
) WITH (autovacuum_enabled = off);

CREATE TABLE movie_kw (
    movie_id INTEGER PRIMARY KEY,
    keyword_id INTEGER NOT NULL,  -- No FOREIGN KEY reference
    year INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

-- Insert tightly correlated data into the dimension table
INSERT INTO keywords (id, keyword, phonetic_code)
SELECT
    i,
    'keyword_' || i,
    CASE WHEN i BETWEEN 26 AND 30 THEN NULL ELSE 'ph_' || i END
FROM generate_series(1, 50) i;

-- Insert data into the fact table with skewed distribution.
-- year is correlated with keyword_id: ids 1-10 -> 2020, 11-20 -> 2021,
-- 21-30 -> 2022.
INSERT INTO movie_kw (movie_id, keyword_id, year)
SELECT
    i,
    CASE
        WHEN i % 100 < 60 THEN (i % 10) + 1      -- 60% keyword_ids 1-10 (6% frequency per keyword)
        WHEN i % 100 < 90 THEN (i % 10) + 11     -- 30% keyword_ids 11-20 (3% frequency per keyword)
        ELSE (i % 10) + 21                       -- 10% keyword_ids 21-30 (1% frequency per keyword)
    END,
    CASE
        WHEN i % 100 < 60 THEN 2020
        WHEN i % 100 < 90 THEN 2021
        ELSE 2022
    END
FROM generate_series(1, 10000) i;

-- Create join MCV statistics on a single filter column (keyword).
CREATE STATISTICS jstats_one_col (mcv)
ON k.keyword
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);

-- Stats definition should be populated.
SELECT s.stxname,
       s.stxrelid::regclass,
       s.stxjoinrels::regclass[],
       s.stxkind
FROM pg_statistic_ext s
WHERE s.stxname = 'jstats_one_col';

SELECT pg_get_statisticsobjdef_columns_from(oid) FROM pg_statistic_ext WHERE stxname = 'jstats_one_col';

-- ANALYZE the joined table (non-anchor).
ANALYZE keywords;

-- The join stats data should not be collected yet.
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'jstats_one_col';

-- ANALYZE the anchor table
ANALYZE movie_kw;

-- Verify pg_stats_ext view resolves column names from the correct table.
-- jstats_one_col tracks keywords.keyword (varno=2 in stxexprs).
-- The view must look up attnum 2 in keywords, not in movie_kw (the anchor).
SELECT exprs
FROM pg_stats_ext
WHERE statistics_name = 'jstats_one_col';

-- The join stats data should now be collected.
SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_one_col'
ORDER BY m.frequency DESC, m.values;

-- Single equality filter on keyword.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND k.id = mk.keyword_id
');

-- IN predicate on keyword.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword IN (''keyword_1'', ''keyword_2'', ''keyword_3'')
      AND k.id = mk.keyword_id
');

-- phonetic_code filter: not covered by the single-column keyword stat,
-- so the planner uses its default estimate.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.phonetic_code = ''ph_1'' AND k.id = mk.keyword_id
');

-- No filter on the joined table: the stat requires at least one covered
-- filter predicate, so the planner uses its default estimate.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.id = mk.keyword_id
');

-- Inequality join: the stat is built on an equality join, so it must not
-- apply when the join operator differs.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.keyword_id > k.id
');

-- Add a stat covering both keyword and phonetic_code.
CREATE STATISTICS jstats_two_col (mcv)
ON k.keyword, k.phonetic_code
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ANALYZE movie_kw;

-- Show the stats in catalog
SELECT s.stxname,
       s.stxrelid::regclass,
       s.stxjoinrels::regclass[],
       s.stxkind
FROM pg_statistic_ext s
WHERE s.stxname = 'jstats_two_col';

SELECT pg_get_statisticsobjdef_columns_from(oid) FROM pg_statistic_ext WHERE stxname = 'jstats_two_col';

-- Verify pg_stats_ext resolves both columns from the joined table.
-- jstats_two_col tracks keywords.keyword and keywords.phonetic_code.
SELECT exprs
FROM pg_stats_ext
WHERE statistics_name = 'jstats_two_col';

SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_two_col'
ORDER BY m.frequency DESC, m.values;

-- phonetic_code filter: now covered by the multi-column stat, estimate improves.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.phonetic_code = ''ph_1'' AND k.id = mk.keyword_id
');

-- Both keyword and phonetic_code: fully covered by the multi-column stat.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_1''
      AND k.id = mk.keyword_id
');

-- Zero MCV match (keyword_1 never pairs with ph_15), falls back to
-- default estimation.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_15''
      AND k.id = mk.keyword_id
');

-- Filters from both tables: keyword from the joined table and year from the
-- anchor.  keyword_1 always pairs with year=2020, but without a join stat
-- covering both tables the planner applies the two filters independently.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- Add a stat with filter columns from both tables.
CREATE STATISTICS jstats_two_tab (mcv)
ON k.keyword, mk.year
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ANALYZE movie_kw;

SELECT s.stxname,
       s.stxrelid::regclass,
       s.stxjoinrels::regclass[],
       s.stxkind
FROM pg_statistic_ext s
WHERE s.stxname = 'jstats_two_tab';

SELECT pg_get_statisticsobjdef_columns_from(oid) FROM pg_statistic_ext WHERE stxname = 'jstats_two_tab';

SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_two_tab'
ORDER BY m.frequency DESC, m.values;

-- Same query with both-table stat: estimate should improve.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- Contradictory filters from both tables: keyword_1 never appears with year 2021.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2021
      AND k.id = mk.keyword_id
');

-- Verify estimates hold when a foreign key constraint is present.
-- The planner estimates FK join selectivity through a separate code path;
-- these tests confirm that join MCV stats are still used.
ALTER TABLE movie_kw ADD CONSTRAINT mk_keyword_fk
    FOREIGN KEY (keyword_id) REFERENCES keywords(id);
ANALYZE movie_kw;

-- Single filter through FK path.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND k.id = mk.keyword_id
');

-- No filter: FK uses 1/ndistinct default.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.id = mk.keyword_id
');

-- Filters from both tables through FK path.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- Contradictory both-table filter through FK path: zero MCV match,
-- falls back to default estimation.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2021
      AND k.id = mk.keyword_id
');

ALTER TABLE movie_kw DROP CONSTRAINT mk_keyword_fk;

-- Verify \dX+ shows join statistics with full definition.
\dX+ jstats_one_col
\dX+ jstats_two_tab

-- Verify \d+ on a table shows join statistics in the footer.
\d+ movie_kw

DROP STATISTICS jstats_one_col;
DROP STATISTICS jstats_two_col;
DROP STATISTICS jstats_two_tab;

-- Reversed column declaration order: jstats_two_tab had {k.keyword, mk.year}
-- (keyrefs = {2, 1}), verify the same works with {mk.year, k.keyword}
-- (keyrefs = {1, 2}).
CREATE STATISTICS jstats_two_tab_rev (mcv)
ON mk.year, k.keyword
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ANALYZE movie_kw;

SELECT s.stxname, pg_get_statisticsobjdef_columns_from(s.oid)
FROM pg_statistic_ext s
WHERE s.stxname = 'jstats_two_tab_rev';

SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- Verify keyrefs mapping is correct under FK path.
ALTER TABLE movie_kw ADD CONSTRAINT mk_keyword_fk
    FOREIGN KEY (keyword_id) REFERENCES keywords(id);
ANALYZE movie_kw;

SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

ALTER TABLE movie_kw DROP CONSTRAINT mk_keyword_fk;
DROP STATISTICS jstats_two_tab_rev;

-- With a lower stattarget the join has more matches than the statistics
-- can keep, so ANALYZE must down-sample.  Verify the result is still sane.
CREATE STATISTICS jstats_sampling (mcv)
ON k.keyword
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ALTER STATISTICS jstats_sampling SET STATISTICS 50;
ANALYZE movie_kw;

-- Verify MCV was built successfully via the sampling path.
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'jstats_sampling';

-- All 30 keywords with matches should be captured even with sampling.
SELECT COUNT(*) AS num_mcvs
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_sampling';

-- Frequency tiers should be preserved: 10 keywords at ~6%, 10 at ~3%,
-- 10 at ~1%.  Use wide bins to absorb sampling variance.
SELECT
    COUNT(*) FILTER (WHERE m.frequency >= 0.04) AS top_tier,
    COUNT(*) FILTER (WHERE m.frequency >= 0.015 AND m.frequency < 0.04) AS mid_tier,
    COUNT(*) FILTER (WHERE m.frequency > 0 AND m.frequency < 0.015) AS low_tier
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_sampling';

DROP STATISTICS jstats_sampling;

-- Hash index: verify that a hash index is accepted and produces reasonable
-- MCV frequencies.
ALTER TABLE keywords DROP CONSTRAINT keywords_pkey;
CREATE INDEX keywords_hash ON keywords USING hash (id);
CREATE STATISTICS jstats_hash (mcv)
ON k.keyword
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ANALYZE movie_kw;

-- Verify MCV was built successfully.
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'jstats_hash';

-- All 30 keywords with matches should be captured.
SELECT COUNT(*) AS num_mcvs
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_hash';

-- Frequency tiers should match btree results: 10 at ~6%, 10 at ~3%, 10 at ~1%.
SELECT
    COUNT(*) FILTER (WHERE m.frequency >= 0.04) AS top_tier,
    COUNT(*) FILTER (WHERE m.frequency >= 0.015 AND m.frequency < 0.04) AS mid_tier,
    COUNT(*) FILTER (WHERE m.frequency > 0 AND m.frequency < 0.015) AS low_tier
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_hash';

-- Equality filter estimate should be accurate with hash index.
SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE k.keyword = ''keyword_1'' AND k.id = mk.keyword_id
');

DROP STATISTICS jstats_hash;
DROP INDEX keywords_hash;

-- check change of unrelated column type does not reset the MCV statistics
ALTER TABLE keywords ADD PRIMARY KEY (id);
CREATE STATISTICS jstats_alter (mcv) ON mk.year
FROM movie_kw mk JOIN keywords k ON (mk.keyword_id = k.id);
ANALYZE movie_kw;

ALTER TABLE keywords ALTER COLUMN phonetic_code TYPE varchar(10);

SELECT d.stxdmcv IS NOT NULL
  FROM pg_statistic_ext s, pg_statistic_ext_data d
 WHERE s.stxname = 'jstats_alter'
   AND d.stxoid = s.oid;

-- check change of stats target column type resets the MCV statistics
ALTER TABLE movie_kw ALTER COLUMN year TYPE numeric;

SELECT estimated < actual / 2 AS mcv_invalidated FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE mk.year = 2020 AND mk.keyword_id = k.id
');

ANALYZE keywords;
ANALYZE movie_kw;

SELECT * FROM check_estimated_rows('
    SELECT * FROM movie_kw mk, keywords k
    WHERE mk.year = 2020 AND mk.keyword_id = k.id
');

DROP STATISTICS jstats_alter;

-- TODO: add tests for ALTER TYPE on a join column (both compatible type
-- changes like integer -> bigint, and incompatible ones like integer ->
-- numeric that currently error with "join statistics require simple equality
-- join conditions").

-- Cleanup base tables
DROP TABLE movie_kw CASCADE;
DROP TABLE keywords CASCADE;

--
-- Range and equality predicates on join MCV statistics.
--
-- range_t1.b and range_t2.e hold the same value on every joined row, so b and
-- e are perfectly correlated across the join.  Values 1..6 occur 80, 50, 30,
-- 20, 12, 8 times.  Per-column stats treat filters on b and e as independent
-- and misestimate the correlated cases; the join MCV captures the correlation.
--
-- One shared query set is run three times: without the statistics (the
-- baseline to beat), with a complete MCV (all six combinations stored), and
-- with an incomplete MCV (only the two most frequent stored).  The join stat
-- is doing its job when the with-statistics estimate is at least as close to
-- the actual as the baseline, most visibly for the multi-column equalities.
CREATE TABLE range_t1 (a int PRIMARY KEY, b int) WITH (autovacuum_enabled = off);
CREATE TABLE range_t2 (d int PRIMARY KEY, e int) WITH (autovacuum_enabled = off);
INSERT INTO range_t1
SELECT i, CASE WHEN i <= 80 THEN 1 WHEN i <= 130 THEN 2 WHEN i <= 160 THEN 3
               WHEN i <= 180 THEN 4 WHEN i <= 192 THEN 5 ELSE 6 END
FROM generate_series(1, 200) i;
INSERT INTO range_t2
SELECT i, CASE WHEN i <= 80 THEN 1 WHEN i <= 130 THEN 2 WHEN i <= 160 THEN 3
               WHEN i <= 180 THEN 4 WHEN i <= 192 THEN 5 ELSE 6 END
FROM generate_series(1, 200) i;
ANALYZE range_t1, range_t2;

--
-- Pass 1: baseline, no join statistics.
--
-- Multi-column range, values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t2.e < 3 AND t1.a = t2.d
');
-- Multi-column range, values {4,5,6}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b > 3 AND t2.e > 3 AND t1.a = t2.d
');
-- Multi-column range, values {1,2,3}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 4 AND t2.e < 4 AND t1.a = t2.d
');
-- Multi-column "<>", excludes value 1.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b <> 1 AND t2.e <> 1 AND t1.a = t2.d
');
-- Multi-column equality on the most common combination.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t2.e = 1 AND t1.a = t2.d
');
-- Multi-column equality on a mid-frequency combination.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t2.e = 3 AND t1.a = t2.d
');
-- Multi-column IN, values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 2) AND t2.e IN (1, 2) AND t1.a = t2.d
');
-- Single-column range, marginalized over the other dimension.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t1.a = t2.d
');
-- Single-column equality, marginalized over the other dimension.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t1.a = t2.d
');
-- Single-column equality on a mid-frequency value.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t1.a = t2.d
');
-- Single-column IN, values {1,3}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 3) AND t1.a = t2.d
');

--
-- Pass 2: complete MCV, all six (b, e) combinations stored (shown below).
--
CREATE STATISTICS jstats_range (mcv) ON t1.b, t2.e
FROM range_t1 t1 JOIN range_t2 t2 ON (t1.a = t2.d);
ANALYZE range_t1;

SELECT m.values, m.nulls, ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_range'
ORDER BY m.frequency DESC, m.values;

-- Multi-column range, values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t2.e < 3 AND t1.a = t2.d
');
-- Multi-column range, values {4,5,6}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b > 3 AND t2.e > 3 AND t1.a = t2.d
');
-- Multi-column range, values {1,2,3}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 4 AND t2.e < 4 AND t1.a = t2.d
');
-- Multi-column "<>", excludes value 1.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b <> 1 AND t2.e <> 1 AND t1.a = t2.d
');
-- Multi-column equality on the most common combination.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t2.e = 1 AND t1.a = t2.d
');
-- Multi-column equality on a mid-frequency combination.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t2.e = 3 AND t1.a = t2.d
');
-- Multi-column IN, values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 2) AND t2.e IN (1, 2) AND t1.a = t2.d
');
-- Single-column range, marginalized over the other dimension.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t1.a = t2.d
');
-- Single-column equality, marginalized over the other dimension.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t1.a = t2.d
');
-- Single-column equality on a mid-frequency value.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t1.a = t2.d
');
-- Single-column IN, values {1,3}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 3) AND t1.a = t2.d
');

-- Additional operator and clause-form coverage (complete MCV).
--
-- "<=" and ">=" (values {1,2} and {4,5,6}).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b <= 2 AND t2.e <= 2 AND t1.a = t2.d
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b >= 4 AND t2.e >= 4 AND t1.a = t2.d
');
-- Constant on the left ("const OP var") is the mirror of "t1.b < 3"; the
-- argument order must be preserved so this also estimates values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE 3 > t1.b AND 3 > t2.e AND t1.a = t2.d
');
-- Single-column range on the other (t2.e) side.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t2.e < 3 AND t1.a = t2.d
');
-- Single-column IN, values {1,2}.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 2) AND t1.a = t2.d
');
-- NULL array elements are dropped, so this behaves like "IN (1, 2)".
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, NULL, 2) AND t1.a = t2.d
');
-- OR is a BoolExpr, not an OpExpr or ScalarArrayOpExpr; falls back.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE (t1.b < 2 OR t1.b > 5) AND t1.a = t2.d
');
-- IS [NOT] NULL is a NullTest, not an OpExpr or ScalarArrayOpExpr; falls back.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IS NOT NULL AND t1.a = t2.d
');
-- ALL (ScalarArrayOpExpr with useOr = false) is rejected; falls back.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < ALL (ARRAY[2, 4]) AND t1.a = t2.d
');
-- An inequality over an array is matched, but the resulting estimate is
-- discarded by the caller's cross-check.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b <= ANY (ARRAY[2, 3]) AND t1.a = t2.d
');

DROP STATISTICS jstats_range;

--
-- Pass 3: incomplete MCV, only the two most frequent combinations stored
-- (values 1 and 2, shown below); values 3..6 fall outside the MCV.
--
CREATE STATISTICS jstats_range (mcv) ON t1.b, t2.e
FROM range_t1 t1 JOIN range_t2 t2 ON (t1.a = t2.d);
ALTER STATISTICS jstats_range SET STATISTICS 2;
ANALYZE range_t1;

SELECT m.values, m.nulls, ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'jstats_range'
ORDER BY m.frequency DESC, m.values;

-- Multi-column range, values {1,2} (both stored).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t2.e < 3 AND t1.a = t2.d
');
-- Multi-column range, values {4,5,6} (all outside the MCV).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b > 3 AND t2.e > 3 AND t1.a = t2.d
');
-- Multi-column range, values {1,2,3} (value 3 is outside the MCV).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 4 AND t2.e < 4 AND t1.a = t2.d
');
-- Multi-column "<>", excludes value 1.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b <> 1 AND t2.e <> 1 AND t1.a = t2.d
');
-- Multi-column equality on the most common combination (stored).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t2.e = 1 AND t1.a = t2.d
');
-- Multi-column equality on a combination outside the MCV.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t2.e = 3 AND t1.a = t2.d
');
-- Multi-column IN, values {1,2} (both stored).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 2) AND t2.e IN (1, 2) AND t1.a = t2.d
');
-- Single-column range, marginalized over the other dimension.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b < 3 AND t1.a = t2.d
');
-- Single-column equality on a stored value.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 1 AND t1.a = t2.d
');
-- Single-column equality on a value outside the MCV.
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b = 3 AND t1.a = t2.d
');
-- Single-column IN straddling the MCV (1 stored, 3 outside).
SELECT * FROM check_estimated_rows('
    SELECT * FROM range_t1 t1, range_t2 t2
    WHERE t1.b IN (1, 3) AND t1.a = t2.d
');

DROP STATISTICS jstats_range;
DROP TABLE range_t1, range_t2;

-- Partial filter coverage with single-table extended stats on the dimension.
--
-- Setup: dimension table where color and shape are perfectly correlated
-- (red & circle, blue & square, etc.).  A single-table extended stat captures
-- this correlation.  The join stat covers only color, not shape.  Queries
-- filter on both.  This tests that partial coverage uses the join stat for
-- the covered filter and correctly accounts for the uncovered filter via
-- the single-table extended stat on other_rel.
CREATE TABLE dim_pc (
    id INTEGER PRIMARY KEY,
    color TEXT NOT NULL,
    shape TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE fact_pc (
    id INTEGER PRIMARY KEY,
    dim_id INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

-- 100 dim rows: 5 colors x 20 rows each, perfectly correlated with shape.
INSERT INTO dim_pc (id, color, shape)
SELECT i,
    CASE (i - 1) / 20
        WHEN 0 THEN 'red'
        WHEN 1 THEN 'blue'
        WHEN 2 THEN 'green'
        WHEN 3 THEN 'yellow'
        ELSE 'white'
    END,
    CASE (i - 1) / 20
        WHEN 0 THEN 'circle'
        WHEN 1 THEN 'square'
        WHEN 2 THEN 'triangle'
        WHEN 3 THEN 'star'
        ELSE 'diamond'
    END
FROM generate_series(1, 100) i;

-- 10000 fact rows with skewed distribution:
-- 50% -> dim ids 1-20 (red/circle), 30% -> 21-40 (blue/square),
-- 20% -> 41-60 (green/triangle), 0% -> 61-100.
INSERT INTO fact_pc (id, dim_id)
SELECT i,
    CASE
        WHEN i % 100 < 50 THEN (i % 20) + 1
        WHEN i % 100 < 80 THEN (i % 20) + 21
        ELSE (i % 20) + 41
    END
FROM generate_series(1, 10000) i;

ANALYZE dim_pc;
ANALYZE fact_pc;

-- Single-table extended stat on dim: captures color & shape correlation.
CREATE STATISTICS dim_pc_ext (mcv) ON color, shape FROM dim_pc;
ANALYZE dim_pc;

-- Verify _columns_from on a single-table stat returns columns + FROM.
SELECT pg_get_statisticsobjdef_columns_from(oid) FROM pg_statistic_ext WHERE stxname = 'dim_pc_ext';

-- Baseline: without join stat, only single-table stats.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_pc f, dim_pc d
    WHERE d.color = ''red'' AND d.shape = ''circle''
      AND f.dim_id = d.id
');

-- Baseline: contradictory filters, without join stat.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_pc f, dim_pc d
    WHERE d.color = ''red'' AND d.shape = ''square''
      AND f.dim_id = d.id
');

-- Now add a join stat covering only color (not shape).
CREATE STATISTICS fact_dim_pc_color (mcv) ON d.color
FROM fact_pc f JOIN dim_pc d ON (f.dim_id = d.id);
ANALYZE fact_pc;

-- red & circle: consistent filters, 50% of fact rows = 5000 actual.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_pc f, dim_pc d
    WHERE d.color = ''red'' AND d.shape = ''circle''
      AND f.dim_id = d.id
');

-- red & square: contradictory filters, 0 actual rows.
-- The single-table stat knows red never pairs with square.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_pc f, dim_pc d
    WHERE d.color = ''red'' AND d.shape = ''square''
      AND f.dim_id = d.id
');

DROP TABLE fact_pc, dim_pc CASCADE;

-- Test that join stats are applied correctly in multi-way joins.

-- Create dimension tables for a star schema: facts joins dim1 and dim2.
CREATE TABLE facts (
    id INTEGER PRIMARY KEY,
    dim1_id INTEGER NOT NULL,
    dim2_id INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE dim1 (
    id INTEGER PRIMARY KEY,
    label TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE dim2 (
    id INTEGER PRIMARY KEY,
    color TEXT NOT NULL
) WITH (autovacuum_enabled = off);

-- dim1: 50 rows
INSERT INTO dim1 (id, label)
SELECT i, 'label_' || i FROM generate_series(1, 50) i;

-- dim2: 20 rows
INSERT INTO dim2 (id, color)
SELECT i, 'color_' || i FROM generate_series(1, 20) i;

-- facts: 10000 rows with skewed distributions to both dimensions.
-- dim1_id: 60% go to ids 1-10 (6% each), 30% to 11-20, 10% to 21-30.
-- dim2_id: 50% go to id 1, 30% to id 2, 20% spread across 3-20.
INSERT INTO facts (id, dim1_id, dim2_id)
SELECT i,
    CASE
        WHEN i % 100 < 60 THEN (i % 10) + 1
        WHEN i % 100 < 90 THEN (i % 10) + 11
        ELSE (i % 10) + 21
    END,
    CASE
        WHEN i % 100 < 50 THEN 1
        WHEN i % 100 < 80 THEN 2
        ELSE (i % 20) + 3
    END
FROM generate_series(1, 10000) i;

ANALYZE facts;
ANALYZE dim1;
ANALYZE dim2;


-- Create join stats on both pairs: facts-dim1 and facts-dim2.
CREATE STATISTICS facts_dim1_stat (mcv) ON d1.label
FROM facts f JOIN dim1 d1 ON (f.dim1_id = d1.id);
ANALYZE facts;

CREATE STATISTICS facts_dim2_stat (mcv) ON d2.color
FROM facts f JOIN dim2 d2 ON (f.dim2_id = d2.id);
ANALYZE facts;

-- Test 1: 2-way baselines. These should use the join stats directly.
-- label_1 -> dim1.id=1 -> 6% of facts = 600 rows.
-- Without stat the planner estimates 10000/50 = 200.
SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f JOIN dim1 d1 ON (f.dim1_id = d1.id)
    WHERE d1.label = ''label_1''
');

-- color_1 -> dim2.id=1 -> 50% of facts = 5000 rows.
-- Without stat the planner estimates 10000/20 = 500.
SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f JOIN dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d2.color = ''color_1''
');

-- Test 2: 3-way join with ONE stat applicable.
-- facts joins dim1 and a plain table (no stat). The facts-dim1 stat
-- should still be found even when one join side is a join rel.
SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f
    JOIN dim2 d2 ON (f.dim2_id = d2.id)
    JOIN dim1 d1 ON (f.dim1_id = d1.id)
    WHERE d1.label = ''label_1''
');

-- Test 3: 3-way join with BOTH stats applicable.
-- facts joins dim1 (filter label_1) and dim2 (filter color_1).
-- Both stats should contribute independently: 10000 * 6% * 50% = 300.
SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f
    JOIN dim1 d1 ON (f.dim1_id = d1.id)
    JOIN dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- Test 4: Same 3-way query, different FROM order. The estimate should
-- be the same regardless of how the tables are listed.
SELECT * FROM check_estimated_rows('
    SELECT * FROM dim2 d2
    JOIN facts f ON (f.dim2_id = d2.id)
    JOIN dim1 d1 ON (f.dim1_id = d1.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- Test 5: Verify that dropping one stat degrades only that dimension.
-- Drop facts-dim2 stat; the dim2 estimate should fall back to default
-- while dim1 still uses the stat.
DROP STATISTICS facts_dim2_stat;

SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f
    JOIN dim1 d1 ON (f.dim1_id = d1.id)
    JOIN dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- Test 6: No stats at all. Verify the partial-stat estimate (test 5) is
-- not worse than the no-stat baseline.
DROP STATISTICS facts_dim1_stat;

SELECT * FROM check_estimated_rows('
    SELECT * FROM facts f
    JOIN dim1 d1 ON (f.dim1_id = d1.id)
    JOIN dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

DROP TABLE facts, dim1, dim2;

-- Multi-column FK: join stats should not alter the FK selectivity.
--
-- A composite FK (fk_a, fk_b) REFERENCES dim(a, b) produces two join
-- clauses. Join stats currently only support single-condition joins, so
-- they cannot cover both FK columns. The FK path should skip the stat
-- entirely.

CREATE TABLE dim_mfk (
    a INTEGER NOT NULL,
    b INTEGER NOT NULL,
    label TEXT NOT NULL,
    PRIMARY KEY (a, b)
) WITH (autovacuum_enabled = off);

CREATE TABLE fact_mfk (
    id INTEGER PRIMARY KEY,
    fk_a INTEGER NOT NULL,
    fk_b INTEGER NOT NULL,
    FOREIGN KEY (fk_a, fk_b) REFERENCES dim_mfk(a, b)
) WITH (autovacuum_enabled = off);

-- dim: 50 rows, a in 1..10, b in 1..5, label skewed by a.
INSERT INTO dim_mfk (a, b, label)
SELECT (i - 1) / 5 + 1, (i - 1) % 5 + 1,
    CASE WHEN (i - 1) / 5 + 1 <= 3 THEN 'hot' ELSE 'cold' END
FROM generate_series(1, 50) i;

-- fact: 10000 rows, fk_a skewed toward low values, fk_b uniform.
INSERT INTO fact_mfk (id, fk_a, fk_b)
SELECT i,
    CASE
        WHEN i % 100 < 60 THEN (i % 3) + 1
        WHEN i % 100 < 90 THEN (i % 3) + 4
        ELSE (i % 4) + 7
    END,
    (i % 5) + 1
FROM generate_series(1, 10000) i;

ANALYZE dim_mfk;
ANALYZE fact_mfk;

-- Baseline: default FK selectivity, no join stat.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_mfk f, dim_mfk d
    WHERE d.label = ''hot'' AND f.fk_a = d.a AND f.fk_b = d.b
');

-- Add join stat covering only one FK column (fk_a = dim.a).
CREATE STATISTICS fact_dim_mfk (mcv) ON d.label
FROM fact_mfk f JOIN dim_mfk d ON (f.fk_a = d.a);
ANALYZE fact_mfk;

-- With join stat: the estimate must stay the same as the baseline.
-- The stat covers only one of the two FK columns, so the FK path should
-- ignore it and fall back to the default 1/ref_tuples.
SELECT * FROM check_estimated_rows('
    SELECT * FROM fact_mfk f, dim_mfk d
    WHERE d.label = ''hot'' AND f.fk_a = d.a AND f.fk_b = d.b
');

DROP TABLE fact_mfk, dim_mfk CASCADE;

--
-- Non-MCV value estimation -- single-key case.
-- Estimates should not be worse than without join stats.
--

CREATE TABLE tail_dim (
    id    INTEGER PRIMARY KEY,
    label TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE tail_fact (
    dim_id INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

INSERT INTO tail_dim VALUES
    (1, 'common_a'), (2, 'common_b'),
    (3, 'other_a'), (4, 'other_b'),
    (5, 'extra_1'), (6, 'extra_2');    -- dimension-only

INSERT INTO tail_fact (dim_id)
SELECT 1 FROM generate_series(1, 90)   -- common_a
UNION ALL SELECT 2 FROM generate_series(1, 70)   -- common_b
UNION ALL SELECT 3 FROM generate_series(1, 25)   -- other_a
UNION ALL SELECT 4 FROM generate_series(1, 15);  -- other_b

ANALYZE tail_dim;
ANALYZE tail_fact;

-- Baselines without join stat.
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact f, tail_dim d
    WHERE d.label IN (''common_a'', ''other_a'') AND d.id = f.dim_id
');

SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact f, tail_dim d
    WHERE d.label = ''other_a'' AND d.id = f.dim_id
');

-- stattarget=2: only the top 2 items land in the MCV.
CREATE STATISTICS jstats_tail (mcv) ON d.label
FROM tail_fact f JOIN tail_dim d ON (f.dim_id = d.id);
ALTER STATISTICS jstats_tail SET STATISTICS 2;
ANALYZE tail_fact;

-- Test 1: IN-list mixing MCV + non-MCV value (same query as baseline).
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact f, tail_dim d
    WHERE d.label IN (''common_a'', ''other_a'') AND d.id = f.dim_id
');

-- Test 2: single non-MCV value (same query as baseline).
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact f, tail_dim d
    WHERE d.label = ''other_a'' AND d.id = f.dim_id
');

DROP STATISTICS jstats_tail;
DROP TABLE tail_fact, tail_dim;

--
-- Non-MCV value estimation -- multi-key case.
-- Estimates should not be worse than without join stats.
--

CREATE TABLE tail_dim2 (
    id    INTEGER PRIMARY KEY,
    color TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE tail_fact2 (
    dim_id   INTEGER NOT NULL,
    category TEXT NOT NULL
) WITH (autovacuum_enabled = off);

INSERT INTO tail_dim2 VALUES
    (1, 'common'), (2, 'other'),
    (3, 'extra_1'), (4, 'extra_2');   -- dimension-only

-- 4 combinations = 2 categories x 2 colors; top 2 are both on 'common'.
INSERT INTO tail_fact2 (dim_id, category)
SELECT 1, 'A' FROM generate_series(1, 60)    -- (common, A)
UNION ALL SELECT 1, 'B' FROM generate_series(1, 55)    -- (common, B)
UNION ALL SELECT 2, 'A' FROM generate_series(1, 43)    -- (other, A)
UNION ALL SELECT 2, 'B' FROM generate_series(1, 42);   -- (other, B)

ANALYZE tail_dim2;
ANALYZE tail_fact2;

-- Baselines without join stat.
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact2 f, tail_dim2 d
    WHERE d.color IN (''common'', ''other'') AND d.id = f.dim_id
');

SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact2 f, tail_dim2 d
    WHERE d.color = ''other'' AND d.id = f.dim_id
');

-- stattarget=2: only (A,common) and (B,common) land in the MCV.
CREATE STATISTICS jstats_tail2 (mcv) ON f.category, d.color
FROM tail_fact2 f JOIN tail_dim2 d ON (f.dim_id = d.id);
ALTER STATISTICS jstats_tail2 SET STATISTICS 2;
ANALYZE tail_fact2;

-- Test 1: IN-list mixing MCV + non-MCV color (same query as baseline).
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact2 f, tail_dim2 d
    WHERE d.color IN (''common'', ''other'') AND d.id = f.dim_id
');

-- Test 2: single non-MCV value (same query as baseline).
SELECT * FROM check_estimated_rows('
    SELECT * FROM tail_fact2 f, tail_dim2 d
    WHERE d.color = ''other'' AND d.id = f.dim_id
');

DROP STATISTICS jstats_tail2;
DROP TABLE tail_fact2, tail_dim2;

--
-- Cross-type join: float4 anchor probing a float8 index.
-- The join uses the float48eq cross-type operator in btree/float_ops.
-- Data is skewed: 80% of fact rows reference 'red' dimension rows.
--
CREATE TABLE ct_dim (id float8 PRIMARY KEY, color text NOT NULL)
    WITH (autovacuum_enabled = off);
CREATE TABLE ct_fact (id serial, dim_id float4 NOT NULL)
    WITH (autovacuum_enabled = off);

-- 15 dimension rows: ids 1-5 are 'red', 6-10 'blue', 11-15 'green'.
INSERT INTO ct_dim SELECT i::float8, CASE WHEN i <= 5 THEN 'red'
                                          WHEN i <= 10 THEN 'blue'
                                          ELSE 'green' END
FROM generate_series(1, 15) i;

-- 500 fact rows: 80% reference red (ids 1-5), 20% reference others.
-- Standard estimator assumes uniform and underestimates the red share.
INSERT INTO ct_fact (dim_id)
SELECT CASE WHEN i % 5 < 4 THEN (i % 5 + 1)::float4
            ELSE (6 + i % 10)::float4 END
FROM generate_series(1, 500) i;

ANALYZE ct_dim;
ANALYZE ct_fact;

-- Baseline: without join stat, standard estimator assumes uniform.
SELECT * FROM check_estimated_rows('
    SELECT * FROM ct_fact f, ct_dim d
    WHERE f.dim_id = d.id AND d.color = ''red''
');

CREATE STATISTICS jstats_crosstype (mcv) ON ct_fact.dim_id, ct_dim.color
FROM ct_fact JOIN ct_dim ON (ct_fact.dim_id = ct_dim.id);

ANALYZE ct_fact;

-- The stat should be populated (stxdmcv IS NOT NULL).
SELECT stxname FROM pg_statistic_ext
WHERE stxname = 'jstats_crosstype'
AND EXISTS (
    SELECT 1 FROM pg_statistic_ext_data d
    WHERE d.stxoid = pg_statistic_ext.oid
    AND d.stxdmcv IS NOT NULL
);

-- With join stat, estimate should improve for the skewed data.
SELECT * FROM check_estimated_rows('
    SELECT * FROM ct_fact f, ct_dim d
    WHERE f.dim_id = d.id AND d.color = ''red''
');

DROP STATISTICS jstats_crosstype;
DROP TABLE ct_fact, ct_dim;

--
-- Same-named tables in different schemas: deparse must disambiguate aliases.
--

CREATE SCHEMA m5_s1;
CREATE SCHEMA m5_s2;

CREATE TABLE m5_s1.t (a int PRIMARY KEY) WITH (autovacuum_enabled = off);
CREATE TABLE m5_s2.t (b int) WITH (autovacuum_enabled = off);
CREATE INDEX ON m5_s2.t (b);

INSERT INTO m5_s1.t SELECT generate_series(1, 100);
INSERT INTO m5_s2.t SELECT generate_series(1, 100);

ANALYZE m5_s1.t;
ANALYZE m5_s2.t;

CREATE STATISTICS m5_jstats (mcv) ON m5_s2.t.b
FROM m5_s1.t JOIN m5_s2.t ON (m5_s1.t.a = m5_s2.t.b);

-- The deparsed DDL must use distinct aliases for identically-named tables.
SELECT pg_get_statisticsobjdef(oid) FROM pg_statistic_ext
WHERE stxname = 'm5_jstats';

DROP STATISTICS m5_jstats;
DROP TABLE m5_s1.t, m5_s2.t;
DROP SCHEMA m5_s1;
DROP SCHEMA m5_s2;

--
-- Privileges for join statistics.
--
-- CREATE requires ownership of the anchor relation and SELECT on the joined
-- relation(s).  ANALYZE refreshes the statistics only if the owner still has
-- SELECT on the joined relation(s); otherwise the refresh is skipped (with a
-- warning), leaving the existing data in place.
--
CREATE ROLE regress_join_stats_user;
CREATE ROLE regress_join_stats_user2;

CREATE TABLE perm_anchor (id int, val int) WITH (autovacuum_enabled = off);
CREATE TABLE perm_dim (id int PRIMARY KEY, label text) WITH (autovacuum_enabled = off);
INSERT INTO perm_anchor SELECT i, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO perm_dim SELECT i, 'l' FROM generate_series(1, 1000) i;
ANALYZE perm_anchor, perm_dim;

GRANT CREATE ON SCHEMA public TO regress_join_stats_user, regress_join_stats_user2;
GRANT SELECT ON perm_anchor, perm_dim TO regress_join_stats_user;

-- not the anchor owner: CREATE fails
SET SESSION AUTHORIZATION regress_join_stats_user;
CREATE STATISTICS perm_jstat (mcv) ON perm_anchor.val
FROM perm_anchor JOIN perm_dim ON (perm_anchor.id = perm_dim.id);
RESET SESSION AUTHORIZATION;
ALTER TABLE perm_anchor OWNER TO regress_join_stats_user;

-- anchor owner, no SELECT on perm_dim: CREATE fails
REVOKE SELECT ON perm_dim FROM regress_join_stats_user;
SET SESSION AUTHORIZATION regress_join_stats_user;
CREATE STATISTICS perm_jstat (mcv) ON perm_anchor.val
FROM perm_anchor JOIN perm_dim ON (perm_anchor.id = perm_dim.id);
RESET SESSION AUTHORIZATION;

-- anchor owner, SELECT on perm_dim: CREATE succeeds
GRANT SELECT ON perm_dim TO regress_join_stats_user;
SET SESSION AUTHORIZATION regress_join_stats_user;
CREATE STATISTICS perm_jstat (mcv) ON perm_anchor.val
FROM perm_anchor JOIN perm_dim ON (perm_anchor.id = perm_dim.id);
RESET SESSION AUTHORIZATION;

-- owner has SELECT: ANALYZE builds the MCV
ANALYZE perm_anchor;
SELECT stxname, (d.stxdmcv IS NOT NULL) AS has_mcv
FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON (d.stxoid = s.oid)
WHERE s.stxname = 'perm_jstat';

-- anchor owner changed: no effect
ALTER TABLE perm_anchor OWNER TO CURRENT_USER;
ANALYZE perm_anchor;

-- stats owner changed to a role without SELECT on perm_dim: ANALYZE skips
ALTER STATISTICS perm_jstat OWNER TO regress_join_stats_user2;
ANALYZE perm_anchor;
SELECT stxname, (d.stxdmcv IS NOT NULL) AS has_mcv
FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON (d.stxoid = s.oid)
WHERE s.stxname = 'perm_jstat';

-- owner granted SELECT on perm_dim: ANALYZE refreshes
GRANT SELECT ON perm_dim TO regress_join_stats_user2;
ANALYZE perm_anchor;

-- owner's SELECT on perm_dim revoked: ANALYZE skips
REVOKE SELECT ON perm_dim FROM regress_join_stats_user2;
ANALYZE perm_anchor;
SELECT stxname, (d.stxdmcv IS NOT NULL) AS has_mcv
FROM pg_statistic_ext s JOIN pg_statistic_ext_data d ON (d.stxoid = s.oid)
WHERE s.stxname = 'perm_jstat';

DROP STATISTICS perm_jstat;
DROP TABLE perm_anchor, perm_dim;
REVOKE CREATE ON SCHEMA public FROM regress_join_stats_user, regress_join_stats_user2;
DROP ROLE regress_join_stats_user, regress_join_stats_user2;

--
-- Leakproof check for join statistics.
--
-- The join MCV estimator applies a filter's operator to the stored MCV values
-- at plan time.  As with single-table MCV stats, a non-leakproof operator must
-- not be applied to those values when the user cannot read the underlying rows,
-- or it could reveal protected values.  Here row-level security hides
-- leak_anchor's rows from the test user; op_leak() reports every pair it is
-- called with, so a leak shows up as a NOTICE.
CREATE ROLE regress_join_leak_user;
CREATE TABLE leak_anchor (a int, b int) WITH (autovacuum_enabled = off);
CREATE TABLE leak_dim (d int PRIMARY KEY, e int) WITH (autovacuum_enabled = off);
INSERT INTO leak_anchor SELECT i, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO leak_dim SELECT i, i % 10 FROM generate_series(1, 1000) i;
CREATE STATISTICS leak_jstat (mcv) ON t1.b, t2.e
FROM leak_anchor t1 JOIN leak_dim t2 ON (t1.a = t2.d);
ANALYZE leak_anchor, leak_dim;

CREATE FUNCTION op_leak(int, int) RETURNS bool
    AS 'BEGIN RAISE NOTICE ''op_leak => %, %'', $1, $2; RETURN $1 < $2; END'
    LANGUAGE plpgsql;
-- The estimator only considers operators with an ordering-index
-- interpretation, so add the leaky operator to an ordering operator family.
CREATE OPERATOR <<< (procedure = op_leak, leftarg = int, rightarg = int,
                     restrict = scalarltsel);
CREATE OPERATOR FAMILY leak_ops USING btree;
ALTER OPERATOR FAMILY leak_ops USING btree ADD OPERATOR 1 <<< (int4, int4);

-- Hide leak_anchor's rows from the test user with a row-level security policy.
ALTER TABLE leak_anchor ENABLE ROW LEVEL SECURITY;
CREATE POLICY leak_anchor_pol ON leak_anchor USING (2 * a < 0);
GRANT SELECT ON leak_anchor, leak_dim TO regress_join_leak_user;

-- As an unprivileged user who cannot read leak_anchor, the query must produce
-- no op_leak NOTICE.
SET SESSION AUTHORIZATION regress_join_leak_user;
SELECT * FROM leak_anchor t1 JOIN leak_dim t2 ON t1.a = t2.d WHERE t1.b <<< 5;
RESET SESSION AUTHORIZATION;

DROP STATISTICS leak_jstat;
DROP OPERATOR FAMILY leak_ops USING btree;
DROP OPERATOR <<< (int4, int4);
DROP FUNCTION op_leak(int, int);
DROP TABLE leak_anchor, leak_dim;
DROP ROLE regress_join_leak_user;

--
-- Auto-generated statistics object name for a join statistics object.
--
-- A join statistics object qualifies each column with its relation name, since
-- a bare column name would not say which relation it came from.  (Single-table
-- naming is covered in stats_ext.)
CREATE TABLE auto_t1 (a INTEGER, b INTEGER, c INTEGER);
CREATE TABLE auto_t2 (d INTEGER, e INTEGER, f INTEGER);
CREATE INDEX ON auto_t1 (a);
CREATE INDEX ON auto_t2 (d);

-- Every column is qualified with its relation, including two columns from the
-- same relation.
CREATE STATISTICS (mcv) ON auto_t1.b, auto_t1.c, auto_t2.e, auto_t2.f
    FROM auto_t1 JOIN auto_t2 ON (auto_t1.a = auto_t2.d);

SELECT stxname FROM pg_statistic_ext
    WHERE stxrelid IN ('auto_t1'::regclass, 'auto_t2'::regclass)
    ORDER BY stxname;

DROP TABLE auto_t1, auto_t2;
