-- Join MCV statistics
--
-- Note: tables for which we check estimated row counts should be created
-- with autovacuum_enabled = off, so that we don't have unstable results
-- from auto-analyze happening when we didn't expect it.
--

-- Each movie has one keyword out of 50: a few keywords are on many movies
-- and most on few or none, which the per-column statistics do not see.
-- Each keyword has its own phonetic_code, NULL for some, and the keyword
-- decides the year.
CREATE TABLE join_mcv_keywords (
    id INTEGER PRIMARY KEY,
    keyword TEXT NOT NULL,
    phonetic_code character varying(5)
) WITH (autovacuum_enabled = off);

-- keyword_id has no foreign key to join_mcv_keywords: the join is estimated
-- from the columns' statistics
CREATE TABLE join_mcv_movies (
    movie_id INTEGER PRIMARY KEY,
    keyword_id INTEGER NOT NULL,
    year INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

INSERT INTO join_mcv_keywords (id, keyword, phonetic_code)
SELECT
    i,
    'keyword_' || i,
    CASE WHEN i BETWEEN 26 AND 30 THEN NULL ELSE 'ph_' || i END
FROM generate_series(1, 50) i;

INSERT INTO join_mcv_movies (movie_id, keyword_id, year)
SELECT
    i,
    CASE
        WHEN i % 100 < 60 THEN (i % 10) + 1
        WHEN i % 100 < 90 THEN (i % 10) + 11
        ELSE (i % 10) + 21
    END,
    CASE
        WHEN i % 100 < 60 THEN 2020
        WHEN i % 100 < 90 THEN 2021
        ELSE 2022
    END
FROM generate_series(1, 10000) i;
ANALYZE join_mcv_keywords, join_mcv_movies;

-- The first table of the FROM clause is the anchor: the statistics object
-- belongs to it, and ANALYZE on it samples the join.  The definition as
-- stored: the anchor in stxrelid, every table in stxjoinrels, and the mcv
-- kind
CREATE STATISTICS join_mcv_movies_keyword_stats (mcv)
ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
SELECT stxrelid::regclass, stxjoinrels::regclass[], stxkind
FROM pg_statistic_ext WHERE stxname = 'join_mcv_movies_keyword_stats';
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_movies_keyword_stats';
\dX+ join_mcv_movies_keyword_stats
-- the anchor table lists its join statistics
\d+ join_mcv_movies
-- pg_stats_ext resolves the column in join_mcv_keywords, not by attribute
-- number in the anchor, where 2 is keyword_id
ANALYZE join_mcv_movies;
SELECT exprs FROM pg_stats_ext
WHERE statistics_name = 'join_mcv_movies_keyword_stats';
DROP STATISTICS join_mcv_movies_keyword_stats;

-- without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND k.id = mk.keyword_id
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword IN (''keyword_1'', ''keyword_2'', ''keyword_3'')
      AND k.id = mk.keyword_id
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_15''
      AND k.id = mk.keyword_id
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.phonetic_code = ''ph_1'' AND k.id = mk.keyword_id
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- join statistics on the keyword: each joined keyword with its fraction of
-- the 10000 joined rows
CREATE STATISTICS join_mcv_movies_keyword_stats (mcv)
ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;
SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_movies_keyword_stats'
ORDER BY m.frequency DESC, m.values;
-- A stored keyword is estimated from its frequency, alone and in a list
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND k.id = mk.keyword_id
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword IN (''keyword_1'', ''keyword_2'', ''keyword_3'')
      AND k.id = mk.keyword_id
');
-- A filter the statistics do not cover is estimated as without them
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.phonetic_code = ''ph_1'' AND k.id = mk.keyword_id
');

-- Join statistics on keyword and phonetic_code store every combination in
-- the join
CREATE STATISTICS join_mcv_movies_keyword_code_stats (mcv)
ON k.keyword, k.phonetic_code
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;
SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_movies_keyword_code_stats'
ORDER BY m.frequency DESC, m.values;
-- phonetic_code is covered now
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.phonetic_code = ''ph_1'' AND k.id = mk.keyword_id
');
-- Both columns
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_1''
      AND k.id = mk.keyword_id
');
-- keyword_1 never pairs with ph_15, and the statistics store every
-- combination in the join, so the estimate is the one-row minimum
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_15''
      AND k.id = mk.keyword_id
');

-- Still no rows with a filter the statistics do not cover added
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1''
      AND k.phonetic_code = ''ph_15''
      AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- Join statistics with a column from each table
CREATE STATISTICS join_mcv_movies_keyword_year_stats (mcv)
ON k.keyword, mk.year
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_movies_keyword_year_stats';
SELECT m.values,
       m.nulls,
       ROUND(m.frequency::numeric, 2) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_movies_keyword_year_stats'
ORDER BY m.frequency DESC, m.values;
-- keyword_1 always goes with 2020, which the statistics store
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');

-- keyword_1 never goes with 2021
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2021
      AND k.id = mk.keyword_id
');
-- The columns may be listed in either order
CREATE STATISTICS join_mcv_movies_year_keyword_stats (mcv)
ON mk.year, k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;
SELECT s.stxname, pg_get_statisticsobjdef_body(s.oid)
FROM pg_statistic_ext s
WHERE s.stxname = 'join_mcv_movies_year_keyword_stats';
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.year = 2020
      AND k.id = mk.keyword_id
');
DROP STATISTICS join_mcv_movies_year_keyword_stats;
DROP STATISTICS join_mcv_movies_keyword_code_stats,
                join_mcv_movies_keyword_year_stats;

-- Verify failures
CREATE STATISTICS join_mcv_bad_no_from (mcv) ON k.keyword;
-- a FROM list with a comma is not supported yet
CREATE STATISTICS join_mcv_bad_comma_list (mcv) ON k.keyword
FROM join_mcv_movies mk, join_mcv_keywords k;
-- nor one with a join among its items
CREATE STATISTICS join_mcv_bad_comma (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id),
     join_mcv_movies mk2;
-- no columns
CREATE STATISTICS join_mcv_bad_no_columns (mcv)
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- an unqualified column name
CREATE STATISTICS join_mcv_bad_unqualified (mcv) ON keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- an expression
CREATE STATISTICS join_mcv_bad_expression (mcv) ON lower(k.keyword)
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- other kinds than mcv
CREATE STATISTICS join_mcv_bad_ndistinct (ndistinct) ON k.keyword, mk.year
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- a condition on a single table would make the statistics describe only the
-- rows satisfying it, alone or with a join condition
CREATE STATISTICS join_mcv_bad_same_table (mcv) ON k.keyword
FROM join_mcv_movies mk
     JOIN join_mcv_keywords k ON (mk.keyword_id = mk.movie_id);
CREATE STATISTICS join_mcv_bad_single_table_filter (mcv) ON k.keyword
FROM join_mcv_movies mk
     JOIN join_mcv_keywords k ON (mk.keyword_id = k.id
                                 AND mk.movie_id = mk.keyword_id);
-- a non-equality join condition
CREATE STATISTICS join_mcv_bad_non_eq (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id > k.id);
-- an outer join
CREATE STATISTICS join_mcv_bad_left (mcv) ON k.keyword
FROM join_mcv_movies mk LEFT JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- a whole-row reference in the join condition
CREATE STATISTICS join_mcv_bad_wholerow (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk = k);
-- a subquery in the FROM clause
CREATE STATISTICS join_mcv_bad_subquery (mcv) ON x.keyword
FROM (SELECT * FROM join_mcv_keywords) x
     JOIN join_mcv_movies mk ON (x.id = mk.keyword_id);
-- a sampled table
CREATE STATISTICS join_mcv_bad_tablesample (mcv) ON k.keyword
FROM join_mcv_movies mk TABLESAMPLE system (10)
     JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
-- a virtual generated column, in the columns or in the join condition
CREATE TABLE join_mcv_vgc (id INTEGER PRIMARY KEY, v INTEGER,
                           g INTEGER GENERATED ALWAYS AS (v * 2) VIRTUAL);
CREATE STATISTICS join_mcv_bad_vgc (mcv) ON v.g
FROM join_mcv_movies mk JOIN join_mcv_vgc v ON (mk.keyword_id = v.id);
CREATE STATISTICS join_mcv_bad_vgc_cond (mcv) ON k.keyword
FROM join_mcv_vgc v JOIN join_mcv_keywords k ON (v.g = k.id);
DROP TABLE join_mcv_vgc;
-- a column of a type without a less-than operator, even alone, as the MCV
-- list is built by sorting
CREATE TABLE join_mcv_xid (id INTEGER PRIMARY KEY, w xid);
CREATE STATISTICS join_mcv_xid_stats (mcv) ON x.w
FROM join_mcv_movies mk JOIN join_mcv_xid x ON (mk.keyword_id = x.id);
CREATE STATISTICS join_mcv_xid_multi_stats (mcv) ON x.w, mk.keyword_id
FROM join_mcv_movies mk JOIN join_mcv_xid x ON (mk.keyword_id = x.id);
DROP TABLE join_mcv_xid;
-- a partitioned table, whichever of the tables it is
CREATE TABLE join_mcv_plain (id int);
CREATE TABLE join_mcv_partitioned (id int, val int) PARTITION BY RANGE (id);
CREATE TABLE join_mcv_partitioned_1 PARTITION OF join_mcv_partitioned
    FOR VALUES FROM (0) TO (100);
CREATE TABLE join_mcv_partitioned_2 PARTITION OF join_mcv_partitioned
    FOR VALUES FROM (100) TO (200);
CREATE INDEX ON join_mcv_partitioned (id);
CREATE STATISTICS join_mcv_part_probed_stats (mcv) ON p.val
FROM join_mcv_plain s JOIN join_mcv_partitioned p ON (s.id = p.id);
CREATE INDEX ON join_mcv_plain (id);
CREATE STATISTICS join_mcv_part_anchor_stats (mcv) ON s.id
FROM join_mcv_partitioned p JOIN join_mcv_plain s ON (p.id = s.id);
DROP TABLE join_mcv_plain, join_mcv_partitioned;
-- a temporary table joined to a permanent one, as other sessions analyze the
-- permanent table but cannot read the temporary one
CREATE TEMP TABLE join_mcv_temp (id INTEGER PRIMARY KEY, label TEXT);
CREATE STATISTICS join_mcv_bad_temp (mcv) ON t.label
FROM join_mcv_movies mk JOIN join_mcv_temp t ON (mk.keyword_id = t.id);
-- also when it is joined only to another table of the join
CREATE STATISTICS join_mcv_bad_temp_3way (mcv) ON t.label
FROM join_mcv_movies mk
     JOIN join_mcv_keywords k ON (mk.keyword_id = k.id)
     JOIN join_mcv_temp t ON (k.id = t.id);
-- but statistics on the temporary table may join a permanent one
CREATE STATISTICS join_mcv_temp_stats (mcv) ON k.keyword
FROM join_mcv_temp t JOIN join_mcv_keywords k ON (t.id = k.id);
DROP TABLE join_mcv_temp;

-- USING is another way of writing an equality join condition
CREATE TABLE join_mcv_keyword_notes (keyword_id INTEGER PRIMARY KEY, note TEXT)
    WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_keyword_notes SELECT id, 'note' FROM join_mcv_keywords;
ANALYZE join_mcv_keyword_notes;
CREATE STATISTICS join_mcv_using_stats (mcv) ON n.note
FROM join_mcv_movies mk JOIN join_mcv_keyword_notes n USING (keyword_id);
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_using_stats';
DROP STATISTICS join_mcv_using_stats;
-- and a join alias is another way of naming the columns of its tables
CREATE STATISTICS join_mcv_join_alias_stats (mcv) ON j.keyword
FROM (join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id)) j;
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_join_alias_stats';
DROP STATISTICS join_mcv_join_alias_stats;
-- a join of three tables is accepted, with a column from each joined table
-- printed with its own table and the tables stored in the order written;
-- ANALYZE warns that such statistics are not built yet
CREATE STATISTICS join_mcv_three_tables_stats (mcv) ON k.keyword, n.note
    FROM join_mcv_movies mk
    JOIN join_mcv_keywords k ON (mk.keyword_id = k.id)
    JOIN join_mcv_keyword_notes n ON (k.id = n.keyword_id);
SELECT stxjoinrels::regclass[], pg_get_statisticsobjdef_body(oid)
FROM pg_statistic_ext WHERE stxname = 'join_mcv_three_tables_stats';
ANALYZE join_mcv_movies;
DROP STATISTICS join_mcv_three_tables_stats;
-- the same join parenthesized: the tables are stored in an order in which
-- each one is joined to an earlier one, here the notes before the keywords
CREATE STATISTICS join_mcv_nested_stats (mcv) ON k.keyword, n.note
    FROM join_mcv_movies mk
    JOIN (join_mcv_keywords k
          JOIN join_mcv_keyword_notes n ON (k.id = n.keyword_id))
    ON (mk.keyword_id = n.keyword_id);
SELECT stxjoinrels::regclass[], pg_get_statisticsobjdef_body(oid)
FROM pg_statistic_ext WHERE stxname = 'join_mcv_nested_stats';
DROP STATISTICS join_mcv_nested_stats;
-- a join condition may only refer to tables joined so far
CREATE STATISTICS join_mcv_bad_forward_ref (mcv) ON k.keyword, n.note
    FROM join_mcv_movies mk
    JOIN join_mcv_keywords k ON (mk.keyword_id = n.keyword_id)
    JOIN join_mcv_keyword_notes n ON (k.id = n.keyword_id);
-- every table must be connected to the others by a join condition
CREATE STATISTICS join_mcv_bad_unconnected (mcv) ON k.keyword, n.note
    FROM join_mcv_movies mk
    JOIN join_mcv_keywords k ON (mk.keyword_id = k.id)
    JOIN join_mcv_keyword_notes n ON (mk.movie_id = k.id);
-- several conditions between the same two tables are accepted, but not
-- built yet either: ANALYZE skips them, and leaves statistics already in
-- place, such as restored ones
CREATE STATISTICS join_mcv_multiclause_stats (mcv) ON k.keyword
FROM join_mcv_movies mk
     JOIN join_mcv_keywords k ON (mk.keyword_id = k.id AND mk.movie_id = k.id);
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_multiclause_stats';
SELECT pg_restore_extended_stats(
    'schemaname', 'public',
    'relname', 'join_mcv_movies',
    'statistics_schemaname', 'public',
    'statistics_name', 'join_mcv_multiclause_stats',
    'inherited', false,
    'most_common_vals', '{{x}}'::text[],
    'most_common_freqs', '{1}'::double precision[],
    'most_common_base_freqs', '{1}'::double precision[]);
SELECT most_common_vals FROM pg_stats_ext
WHERE statistics_name = 'join_mcv_multiclause_stats';
ANALYZE join_mcv_movies;
SELECT most_common_vals FROM pg_stats_ext
WHERE statistics_name = 'join_mcv_multiclause_stats';
DROP STATISTICS join_mcv_multiclause_stats;
-- without a list of kinds, join statistics get every kind they support,
-- which is only mcv, whatever the number of columns
CREATE STATISTICS join_mcv_kinds_stats_1 ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
CREATE STATISTICS join_mcv_kinds_stats_2 ON k.keyword, mk.year
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
SELECT stxname, stxkind FROM pg_statistic_ext
WHERE stxname LIKE 'join_mcv_kinds%' ORDER BY stxname;
DROP STATISTICS join_mcv_kinds_stats_1, join_mcv_kinds_stats_2;

-- check statistics are dropped when a column of theirs is, whichever table
-- it belongs to, and when a join column is
CREATE STATISTICS join_mcv_dep_note_stats (mcv) ON n.note
FROM join_mcv_movies mk JOIN join_mcv_keyword_notes n USING (keyword_id);
CREATE STATISTICS join_mcv_dep_year_stats (mcv) ON mk.year
FROM join_mcv_movies mk JOIN join_mcv_keyword_notes n USING (keyword_id);
ALTER TABLE join_mcv_keyword_notes DROP COLUMN note;
\d join_mcv_movies
ALTER TABLE join_mcv_keyword_notes DROP COLUMN keyword_id;
\d join_mcv_movies
DROP TABLE join_mcv_keyword_notes;
-- but dropping the operator of their join condition needs CASCADE
CREATE OPERATOR === (FUNCTION = int4eq, LEFTARG = int, RIGHTARG = int,
                     COMMUTATOR = ===);
-- join conditions need a btree equality operator
CREATE OPERATOR FAMILY join_mcv_dep_ops USING btree;
ALTER OPERATOR FAMILY join_mcv_dep_ops USING btree
    ADD OPERATOR 3 === (int, int);
CREATE STATISTICS join_mcv_dep_op_stats (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id === k.id);
DROP OPERATOR === (int, int);
DROP OPERATOR === (int, int) CASCADE;
DROP OPERATOR FAMILY join_mcv_dep_ops USING btree;

-- check dropping a table in the middle of a join of 60 tables drops the
-- statistics (the statements are long, so they are not echoed)
\set ECHO none
SELECT format('CREATE TABLE join_many_%s (k int); CREATE INDEX ON join_many_%s (k);', i, i)
FROM generate_series(1, 60) i
\gexec
SELECT 'CREATE STATISTICS join_many_stats (mcv) ON t1.k, t60.k FROM join_many_1 t1 ' ||
       string_agg(format('JOIN join_many_%s t%s ON (t%s.k = t%s.k)', i, i, i - 1, i),
                  ' ' ORDER BY i)
FROM generate_series(2, 60) i
\gexec
\set ECHO all
SELECT stxname FROM pg_statistic_ext WHERE stxname = 'join_many_stats';
DROP TABLE join_many_30;
SELECT stxname FROM pg_statistic_ext WHERE stxname = 'join_many_stats';
\set ECHO none
SELECT format('DROP TABLE join_many_%s', i)
FROM generate_series(1, 60) i WHERE i <> 30
\gexec
\set ECHO all

-- check the frequencies are fractions of the joined rows, scaled by the
-- size of the join: val = 'hit' is most of the probed table but few of the
-- joined rows, nearly all of which join id 10
CREATE TABLE join_mcv_scale (id int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_scale_indexed (id int, val text)
    WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_scale
SELECT CASE WHEN g <= 9 THEN g ELSE 10 END FROM generate_series(1, 1000) g;
INSERT INTO join_mcv_scale_indexed
SELECT g, CASE WHEN g <= 9 THEN 'hit' ELSE 'miss' END
FROM generate_series(1, 10) g;
CREATE INDEX ON join_mcv_scale_indexed (id);
ANALYZE join_mcv_scale, join_mcv_scale_indexed;

-- Without join statistics: the filter and the join are treated as
-- independent
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_scale s
      JOIN join_mcv_scale_indexed i ON (s.id = i.id)
     WHERE i.val = ''hit''');

-- With join statistics
CREATE STATISTICS join_mcv_scale_stats (mcv) ON i.val
FROM join_mcv_scale s JOIN join_mcv_scale_indexed i ON (s.id = i.id);
ANALYZE join_mcv_scale;
SELECT m.values, m.frequency, m.base_frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_scale_stats'
ORDER BY m.frequency DESC;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_scale s
      JOIN join_mcv_scale_indexed i ON (s.id = i.id)
     WHERE i.val = ''hit''');

DROP TABLE join_mcv_scale, join_mcv_scale_indexed;

-- check statistics of two tables are used when a third table is joined to
-- either of them, in any order; the third table has one row per id, so it
-- changes neither the rows of the join nor the filter
CREATE TABLE join_mcv_three (id int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_three_indexed (id int, y int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_three_indexed (id);
CREATE TABLE join_mcv_three_other (id int) WITH (autovacuum_enabled = off);

-- y = 1 in a few rows
INSERT INTO join_mcv_three_indexed
SELECT g, (g <= 10)::int FROM generate_series(1, 100) g;

-- most rows join the rows where y = 1
INSERT INTO join_mcv_three
SELECT CASE WHEN g <= 900 THEN 1 + g % 10 ELSE 11 + g % 90 END
FROM generate_series(1, 1000) g;

-- 100 rows, one for each id
INSERT INTO join_mcv_three_other SELECT g FROM generate_series(1, 100) g;

ANALYZE join_mcv_three, join_mcv_three_indexed, join_mcv_three_other;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_three_other o
      JOIN join_mcv_three f ON (o.id = f.id)
      JOIN join_mcv_three_indexed i ON (f.id = i.id)
     WHERE i.y = 1');

CREATE STATISTICS join_mcv_three_stats (mcv) ON i.y
FROM join_mcv_three f JOIN join_mcv_three_indexed i ON (f.id = i.id);
ANALYZE join_mcv_three;

-- The two tables of the statistics alone
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_three f
      JOIN join_mcv_three_indexed i ON (f.id = i.id)
     WHERE i.y = 1');

-- With the third table joined to either table of the statistics: the
-- estimate is the same whichever pair the planner joins first
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_three_other o
      JOIN join_mcv_three f ON (o.id = f.id)
      JOIN join_mcv_three_indexed i ON (f.id = i.id)
     WHERE i.y = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_three_other o
      JOIN join_mcv_three_indexed i ON (o.id = i.id)
      JOIN join_mcv_three f ON (f.id = i.id)
     WHERE i.y = 1');

DROP TABLE join_mcv_three, join_mcv_three_indexed, join_mcv_three_other;

-- check statistics correct a foreign key's estimate for a filter: the
-- foreign key does not account for NULL keys, and that error stays
CREATE TABLE join_mcv_fk_null_indexed (id int PRIMARY KEY, y int)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_fk_null (id int REFERENCES join_mcv_fk_null_indexed (id))
    WITH (autovacuum_enabled = off);

-- y = 1 in a few rows
INSERT INTO join_mcv_fk_null_indexed
SELECT g, (g <= 10)::int FROM generate_series(1, 100) g;

-- some rows reference no row, and most of the others reference the rows
-- where y = 1
INSERT INTO join_mcv_fk_null
SELECT CASE WHEN g <= 200 THEN NULL
            WHEN g <= 920 THEN 1 + g % 10
            ELSE 11 + g % 80 END
FROM generate_series(1, 1000) g;

ANALYZE join_mcv_fk_null, join_mcv_fk_null_indexed;

-- Without join statistics, with and without a filter
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk_null s
      JOIN join_mcv_fk_null_indexed i ON (s.id = i.id)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk_null s
      JOIN join_mcv_fk_null_indexed i ON (s.id = i.id)
     WHERE i.y = 1');

-- With join statistics, which correct the foreign key's estimate for the
-- filter only: the remaining error is the foreign key's own, as it does not
-- account for the NULL keys
CREATE STATISTICS join_mcv_fk_null_stats (mcv) ON i.y
FROM join_mcv_fk_null s JOIN join_mcv_fk_null_indexed i ON (s.id = i.id);
ANALYZE join_mcv_fk_null;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk_null s
      JOIN join_mcv_fk_null_indexed i ON (s.id = i.id)
     WHERE i.y = 1');

DROP TABLE join_mcv_fk_null, join_mcv_fk_null_indexed;

-- check estimates for values stored in the join statistics, and for values
-- not stored there
CREATE TABLE join_mcv_stored_indexed (id int PRIMARY KEY, b int, c int)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_stored (dim_id int, a int)
    WITH (autovacuum_enabled = off);

-- common values of b, single rows, and a value no row joins
INSERT INTO join_mcv_stored_indexed
SELECT i, CASE WHEN i <= 30 THEN 1 WHEN i <= 50 THEN 2 WHEN i = 51 THEN 3
               WHEN i = 52 THEN 4 WHEN i <= 72 THEN 5 ELSE 6 END,
       1 + i % 4
FROM generate_series(1, 80) i;

-- a is never below c
INSERT INTO join_mcv_stored
SELECT dim.id, dim.c + (g % 3 = 0)::int
FROM (VALUES (1, 30, 70), (31, 20, 50), (51, 1, 25), (52, 1, 15), (53, 20, 6))
       AS v (first_id, nrows, njoined),
     generate_series(1, v.njoined) g,
     join_mcv_stored_indexed dim
WHERE dim.id = v.first_id + g % v.nrows;

-- 10 rows that join nothing
INSERT INTO join_mcv_stored SELECT NULL, 1 FROM generate_series(1, 10);

ANALYZE join_mcv_stored_indexed, join_mcv_stored;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b IN (1, 3)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 3');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 6');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b > 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a = 2 AND dim.c = 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c = 4');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a > 2 AND dim.c > 2');

-- With join statistics that store every value of b and every (a, c)
-- combination in the join
CREATE STATISTICS join_mcv_stored_b_stats (mcv) ON dim.b
FROM join_mcv_stored f JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id);
CREATE STATISTICS join_mcv_stored_a_c_stats (mcv) ON f.a, dim.c
FROM join_mcv_stored f JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id);
ANALYZE join_mcv_stored;
SELECT s.stxname, m.values, round(m.frequency::numeric, 3) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname IN ('join_mcv_stored_b_stats', 'join_mcv_stored_a_c_stats')
ORDER BY s.stxname, m.frequency DESC;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b IN (1, 3)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 3');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 6');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b > 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a = 2 AND dim.c = 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c = 4');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a > 2 AND dim.c > 2');

-- With lower statistics targets, only the most common values are stored;
-- the joined rows all fit in the sample, so the statistics are the same on
-- every run
ALTER STATISTICS join_mcv_stored_b_stats SET STATISTICS 2;
ALTER STATISTICS join_mcv_stored_a_c_stats SET STATISTICS 3;
ANALYZE join_mcv_stored;
SELECT s.stxname, m.values, round(m.frequency::numeric, 3) AS frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname IN ('join_mcv_stored_b_stats', 'join_mcv_stored_a_c_stats')
ORDER BY s.stxname, m.frequency DESC;
-- A stored value.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 1');
-- One value stored and one not: the frequency of b = 1, plus the estimate
-- without join statistics for b = 3
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b IN (1, 3)');
-- A value not stored gets the estimate without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 3');
-- The statistics no longer show that b = 6 joins nothing, so the estimate
-- without join statistics is kept
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 6');
-- A range no stored value satisfies: the estimate is capped at the joined
-- rows of the values not stored
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b > 2');
-- A combination not stored gets the estimate without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a = 2 AND dim.c = 2');
-- (4, 4) is stored; the other combinations with c = 4 get the estimate
-- without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c = 4');
-- A range: (3, 3) and (4, 4) are stored, and the other combinations it
-- selects get the estimate without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_stored f
      JOIN join_mcv_stored_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a > 2 AND dim.c > 2');

-- Leave join_mcv_stored, join_mcv_stored_indexed and their statistics in
-- place for pg_upgrade testing.

-- check a value stored at a frequency much lower in the join than in its
-- table: b = 0 is 30 percent of the dimension rows but 1 percent of the
-- joined rows, the most common value there
CREATE TABLE join_mcv_diluted (dim_id int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_diluted_indexed (id int PRIMARY KEY, b int)
    WITH (autovacuum_enabled = off);

-- b = 0 in many rows, and a distinct b in each other row
INSERT INTO join_mcv_diluted_indexed
SELECT g, CASE WHEN g <= 90 THEN 0 ELSE g END FROM generate_series(1, 300) g;

-- few rows reference the rows where b = 0; the others reference more values
-- of b than the statistics store
INSERT INTO join_mcv_diluted
SELECT CASE WHEN g <= 10 THEN g ELSE 91 + g % 210 END
FROM generate_series(1, 1000) g;

ANALYZE join_mcv_diluted, join_mcv_diluted_indexed;

-- Without join statistics: b = 0 is 30 percent of the dimension table
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_diluted f
      JOIN join_mcv_diluted_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 0');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_diluted f
      JOIN join_mcv_diluted_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b < 1');

-- With join statistics, which store b = 0 at 1 percent of the joined rows
CREATE STATISTICS join_mcv_diluted_stats (mcv) ON dim.b
FROM join_mcv_diluted f
     JOIN join_mcv_diluted_indexed dim ON (f.dim_id = dim.id);
ANALYZE join_mcv_diluted;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_diluted f
      JOIN join_mcv_diluted_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 0');
-- A range selecting that value alone is measured the same way
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_diluted f
      JOIN join_mcv_diluted_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b < 1');

DROP TABLE join_mcv_diluted, join_mcv_diluted_indexed;

-- check a filter on the anchor table expected to select less than one row
-- is corrected as selecting one row, as a filter on the probed table is,
-- since the planner estimates at least one row for a table
CREATE TABLE join_mcv_rare (id int, a int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_rare_indexed (id int) WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_rare_indexed (id);

-- half the rows have id 1
INSERT INTO join_mcv_rare_indexed
SELECT CASE WHEN g <= 500 THEN 1 ELSE g - 499 END
FROM generate_series(1, 1000) g;

-- the one row with a = -10 references id 1; the others, with a = 0,
-- reference one row each
INSERT INTO join_mcv_rare
SELECT CASE WHEN g = 1 THEN 1 ELSE 2 + g % 500 END,
       CASE WHEN g = 1 THEN -10 ELSE 0 END
FROM generate_series(1, 1000) g;

ANALYZE join_mcv_rare, join_mcv_rare_indexed;

-- Without join statistics: a = -10, and a < -5 with no histogram to
-- estimate it from, are each expected to select one row
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_rare s
      JOIN join_mcv_rare_indexed i ON (s.id = i.id)
     WHERE s.a = -10');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_rare s
      JOIN join_mcv_rare_indexed i ON (s.id = i.id)
     WHERE s.a < -5');

-- With join statistics, which store that a third of the joined rows have
-- a = -10: the two filters get the same estimate
CREATE STATISTICS join_mcv_rare_stats (mcv) ON s.a
FROM join_mcv_rare s JOIN join_mcv_rare_indexed i ON (s.id = i.id);
ANALYZE join_mcv_rare;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_rare s
      JOIN join_mcv_rare_indexed i ON (s.id = i.id)
     WHERE s.a = -10');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_rare s
      JOIN join_mcv_rare_indexed i ON (s.id = i.id)
     WHERE s.a < -5');

DROP TABLE join_mcv_rare, join_mcv_rare_indexed;

-- check which filter clauses the join statistics can estimate
CREATE TABLE join_mcv_filters_indexed (id int PRIMARY KEY, b int, c int, d bool)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_filters (dim_id int, a int)
    WITH (autovacuum_enabled = off);

-- c NULL in some rows, d true in fewer
INSERT INTO join_mcv_filters_indexed
SELECT i, 1 + i % 4, NULLIF(i % 5, 0), i % 10 = 0
FROM generate_series(1, 40) i;

-- a third of the rows join the rows where c is NULL; a is never below b
INSERT INTO join_mcv_filters
SELECT dim.id, dim.b + (g % 3 = 0)::int
FROM generate_series(1, 600) g
JOIN join_mcv_filters_indexed dim
  ON dim.id = CASE WHEN g <= 200 THEN 5 * (1 + g % 8) ELSE 1 + g % 40 END;

ANALYZE join_mcv_filters_indexed, join_mcv_filters;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a > 2 AND dim.b > 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE 2 < f.a AND 2 < dim.b');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a <> 1 AND dim.b <> 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a IN (1, 2) AND dim.b = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a IN (1, NULL, 2) AND dim.b = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a <= ANY (ARRAY[1, 2]) AND dim.b = 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a < ALL (ARRAY[3, 4]) AND dim.b = 2');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c NOT IN (1, 2)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE (f.a = 1 OR f.a = 5) AND dim.b = 4');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE (dim.c = 1 OR dim.c IS NULL)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c IS NULL');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c IS NOT NULL');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.d');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE NOT dim.d');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.d IS TRUE');

-- With join statistics that store every combination in the join
CREATE STATISTICS join_mcv_filters_stats (mcv) ON f.a, dim.b, dim.c, dim.d
FROM join_mcv_filters f
     JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id);
ANALYZE join_mcv_filters;
SELECT count(*) AS items, round(sum(m.frequency)::numeric, 2) AS total_frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_filters_stats';
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a > 2 AND dim.b > 2');
-- A constant on the left: the same as "f.a > 2 AND dim.b > 2".
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE 2 < f.a AND 2 < dim.b');
-- "<>" on both columns.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a <> 1 AND dim.b <> 1');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a IN (1, 2) AND dim.b = 1');
-- NULL array elements are dropped, so this is the same as "f.a IN (1, 2)".
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a IN (1, NULL, 2) AND dim.b = 1');
-- An inequality over an array: the same as "f.a IN (1, 2)" here.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a <= ANY (ARRAY[1, 2]) AND dim.b = 1');
-- ALL: the same as "f.a < 3".
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE f.a < ALL (ARRAY[3, 4]) AND dim.b = 2');
-- NOT IN is "<> ALL".
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c NOT IN (1, 2)');
-- OR of filters on one table.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE (f.a = 1 OR f.a = 5) AND dim.b = 4');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE (dim.c = 1 OR dim.c IS NULL)');
-- IS [NOT] NULL.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c IS NULL');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.c IS NOT NULL');
-- A boolean column, alone and negated.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.d');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE NOT dim.d');
-- IS TRUE is not a form the statistics match: estimated as without them.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_filters f
      JOIN join_mcv_filters_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.d IS TRUE');

-- Leave join_mcv_filters, join_mcv_filters_indexed and their statistics in
-- place for pg_upgrade testing.

-- check a filter the join statistics do not cover is estimated from the
-- probed table's own statistics: color and shape always go together, and
-- the join statistics cover color only
CREATE TABLE join_mcv_partial_indexed (
    id INTEGER PRIMARY KEY,
    color TEXT NOT NULL,
    shape TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE join_mcv_partial (
    id INTEGER PRIMARY KEY,
    dim_id INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

-- each color with its own shape
INSERT INTO join_mcv_partial_indexed (id, color, shape)
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

-- half the rows reference the red rows, none the yellow or white ones
INSERT INTO join_mcv_partial (id, dim_id)
SELECT i,
    CASE
        WHEN i % 100 < 50 THEN (i % 20) + 1
        WHEN i % 100 < 80 THEN (i % 20) + 21
        ELSE (i % 20) + 41
    END
FROM generate_series(1, 10000) i;

ANALYZE join_mcv_partial_indexed;
ANALYZE join_mcv_partial;

-- Single-table statistics on the probed table know that color and shape go
-- together
CREATE STATISTICS join_mcv_partial_single_stats (mcv) ON color, shape
FROM join_mcv_partial_indexed;
ANALYZE join_mcv_partial_indexed;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_partial f, join_mcv_partial_indexed d
    WHERE d.color = ''red'' AND d.shape = ''circle''
      AND f.dim_id = d.id
');

SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_partial f, join_mcv_partial_indexed d
    WHERE d.color = ''red'' AND d.shape = ''square''
      AND f.dim_id = d.id
');

-- With join statistics covering color only
CREATE STATISTICS join_mcv_partial_stats (mcv) ON d.color
FROM join_mcv_partial f JOIN join_mcv_partial_indexed d ON (f.dim_id = d.id);
ANALYZE join_mcv_partial;

-- red and circle go together: the statistics measure the pair
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_partial f, join_mcv_partial_indexed d
    WHERE d.color = ''red'' AND d.shape = ''circle''
      AND f.dim_id = d.id
');

-- red and square never go together: the estimate is the one without join
-- statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_partial f, join_mcv_partial_indexed d
    WHERE d.color = ''red'' AND d.shape = ''square''
      AND f.dim_id = d.id
');

DROP TABLE join_mcv_partial, join_mcv_partial_indexed CASCADE;

-- check two statistics objects are used in one join, one for each
-- dimension table of a fact table
CREATE TABLE join_mcv_star (
    id INTEGER PRIMARY KEY,
    dim1_id INTEGER NOT NULL,
    dim2_id INTEGER NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE join_mcv_star_dim1 (
    id INTEGER PRIMARY KEY,
    label TEXT NOT NULL
) WITH (autovacuum_enabled = off);

CREATE TABLE join_mcv_star_dim2 (
    id INTEGER PRIMARY KEY,
    color TEXT NOT NULL
) WITH (autovacuum_enabled = off);

-- join_mcv_star_dim1: 50 rows
INSERT INTO join_mcv_star_dim1 (id, label)
SELECT i, 'label_' || i FROM generate_series(1, 50) i;

-- join_mcv_star_dim2: 20 rows
INSERT INTO join_mcv_star_dim2 (id, color)
SELECT i, 'color_' || i FROM generate_series(1, 20) i;

-- skewed to both dimensions: dim1_id mostly among the first ten ids,
-- dim2_id mostly id 1, and some rows reference no dim2 row
INSERT INTO join_mcv_star (id, dim1_id, dim2_id)
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

ANALYZE join_mcv_star;
ANALYZE join_mcv_star_dim1;
ANALYZE join_mcv_star_dim2;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_star f
    JOIN join_mcv_star_dim1 d1 ON (f.dim1_id = d1.id)
    JOIN join_mcv_star_dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- With join statistics for each pair
CREATE STATISTICS join_mcv_star_dim1_stats (mcv) ON d1.label
FROM join_mcv_star f JOIN join_mcv_star_dim1 d1 ON (f.dim1_id = d1.id);
ANALYZE join_mcv_star;

CREATE STATISTICS join_mcv_star_dim2_stats (mcv) ON d2.color
FROM join_mcv_star f JOIN join_mcv_star_dim2 d2 ON (f.dim2_id = d2.id);
ANALYZE join_mcv_star;

-- Three-way join with both objects applicable: label_1 and color_1 go
-- together, which neither object knows, so the two corrections combine as
-- independent
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_star f
    JOIN join_mcv_star_dim1 d1 ON (f.dim1_id = d1.id)
    JOIN join_mcv_star_dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- Dropping the color object: label_1 is still measured, color_1 is
-- estimated as 1 color in 20
DROP STATISTICS join_mcv_star_dim2_stats;

SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_star f
    JOIN join_mcv_star_dim1 d1 ON (f.dim1_id = d1.id)
    JOIN join_mcv_star_dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

-- No statistics at all
DROP STATISTICS join_mcv_star_dim1_stats;

SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_star f
    JOIN join_mcv_star_dim1 d1 ON (f.dim1_id = d1.id)
    JOIN join_mcv_star_dim2 d2 ON (f.dim2_id = d2.id)
    WHERE d1.label = ''label_1'' AND d2.color = ''color_1''
');

DROP TABLE join_mcv_star, join_mcv_star_dim1, join_mcv_star_dim2;

-- check an inequality join is not the condition the statistics were built
-- on, so the estimate is the one without statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.keyword_id > k.id
');
DROP STATISTICS join_mcv_movies_keyword_stats;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_movies mk, join_mcv_keywords k
    WHERE k.keyword = ''keyword_1'' AND mk.keyword_id > k.id
');

-- check statistics on a self-join
CREATE TABLE join_mcv_self (
    id INTEGER PRIMARY KEY,
    parent_id INTEGER,
    a INTEGER NOT NULL
) WITH (autovacuum_enabled = off);
-- the 100 rows with a = 1 are the parents of the rows with a = 0
INSERT INTO join_mcv_self SELECT i, NULL, 1 FROM generate_series(1, 100) i;
INSERT INTO join_mcv_self
SELECT i, i % 100 + 1, 0 FROM generate_series(101, 10000) i;
ANALYZE join_mcv_self;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_self c JOIN join_mcv_self p ON (c.parent_id = p.id)
    WHERE c.a = 0 AND p.a = 1
');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_self c JOIN join_mcv_self p ON (c.parent_id = p.id)
    WHERE c.a = 1 AND p.a = 0
');

-- With join statistics
CREATE STATISTICS join_mcv_self_stats (mcv) ON c.a, p.a
FROM join_mcv_self c JOIN join_mcv_self p ON (c.parent_id = p.id);
ANALYZE join_mcv_self;
-- every row with a = 0 has a parent with a = 1
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_self c JOIN join_mcv_self p ON (c.parent_id = p.id)
    WHERE c.a = 0 AND p.a = 1
');
-- and no row with a = 1 has a parent with a = 0
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_self c JOIN join_mcv_self p ON (c.parent_id = p.id)
    WHERE c.a = 1 AND p.a = 0
');

-- Leave join_mcv_self and join_mcv_self_stats in place for pg_upgrade
-- testing.

-- check join statistics are not used for a join of their tables on more
-- than one condition, and what counts as a condition
CREATE TABLE join_mcv_two (a int, b int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_two_indexed (a int, b int, y int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_two_indexed (a);
CREATE INDEX ON join_mcv_two_indexed (b);

-- every pair (a, b) once, with y = 1 where a or b is 1
INSERT INTO join_mcv_two_indexed
SELECT a, b, (a = 1 OR b = 1)::int
FROM generate_series(1, 10) a, generate_series(1, 10) b;

-- most rows join (1, 1), where y = 1
INSERT INTO join_mcv_two
SELECT CASE WHEN g <= 900 THEN 1 ELSE 2 + g % 9 END,
       CASE WHEN g <= 900 THEN 1 ELSE 2 + (g / 9) % 9 END
FROM generate_series(1, 1000) g;

ANALYZE join_mcv_two, join_mcv_two_indexed;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      JOIN join_mcv_two_indexed i ON (s.a = i.a AND s.b = i.b)
     WHERE i.y = 1');

-- With join statistics for one of the conditions
CREATE STATISTICS join_mcv_two_a_stats (mcv) ON i.y
FROM join_mcv_two s JOIN join_mcv_two_indexed i ON (s.a = i.a);
ANALYZE join_mcv_two;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      JOIN join_mcv_two_indexed i ON (s.a = i.a AND s.b = i.b)
     WHERE i.y = 1');

-- A condition equal to a constant becomes a restriction on each table rather
-- than a join condition, so the join statistics are used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      JOIN join_mcv_two_indexed i ON (s.a = i.a AND s.b = i.b)
     WHERE i.y = 1 AND i.b = 1');

-- Expressions over both tables, with join statistics for one of the
-- conditions.  An expression that is only sorted on is not a join
-- condition, so the join statistics are used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      JOIN join_mcv_two_indexed i ON (s.a = i.a)
     WHERE i.y = 1
     ORDER BY s.b + i.b');

-- An expression equal to the join key is a second join condition, so the
-- join statistics are not used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      JOIN join_mcv_two_indexed i ON (s.a = i.a AND i.a = s.b + i.b - 1)
     WHERE i.y = 1');

-- Outer joins, with join statistics for one of the conditions
CREATE TABLE join_mcv_third (k int) WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_third SELECT g FROM generate_series(1, 1000) g;
ANALYZE join_mcv_third;

-- A filter on the anchor table in the join condition of a left join is not a
-- join condition, so the join statistics are used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      LEFT JOIN join_mcv_two_indexed i ON (s.a = i.a AND i.y = 1 AND s.b = 1)');

-- A condition on both tables above a left join is a second join condition,
-- so the join statistics are not used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_two s
      LEFT JOIN join_mcv_two_indexed i ON (s.a = i.a AND i.y = 1)
     WHERE (i.b = s.b OR i.b IS NULL)');

-- One condition, on a column nullable by a full join below, so the join
-- statistics are used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_third t
      FULL JOIN join_mcv_two s ON (t.k = s.a)
      LEFT JOIN join_mcv_two_indexed i ON (s.a = i.a AND i.y = 1)');

-- The same with a left join below, for which the planner keeps a copy of
-- the join clause for each join order
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_third t
      LEFT JOIN join_mcv_two s ON (t.k = s.a)
      LEFT JOIN join_mcv_two_indexed i ON (s.a = i.a AND i.y = 1)');

DROP TABLE join_mcv_two, join_mcv_two_indexed, join_mcv_third;

-- check a foreign key with another condition counts as two conditions
CREATE TABLE join_mcv_fk_indexed (id int PRIMARY KEY, b int, y int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_fk_indexed (b);
CREATE TABLE join_mcv_fk (id int REFERENCES join_mcv_fk_indexed (id), b int)
    WITH (autovacuum_enabled = off);

-- groups of ten rows by b, with y = 1 only in the group b = 0
INSERT INTO join_mcv_fk_indexed
SELECT g, (g - 1) / 10, (g <= 10)::int FROM generate_series(1, 200) g;

-- few rows have the b of the row they reference
INSERT INTO join_mcv_fk
SELECT g % 10 + 1, (g / 10) % 20 FROM generate_series(0, 999) g;
INSERT INTO join_mcv_fk
SELECT i.id, CASE WHEN g % 20 = 0 THEN i.b ELSE 0 END
FROM generate_series(0, 999) g
JOIN join_mcv_fk_indexed i ON (i.id = 11 + g % 190);

ANALYZE join_mcv_fk, join_mcv_fk_indexed;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk s
      JOIN join_mcv_fk_indexed i ON (s.id = i.id AND s.b = i.b)
     WHERE i.y = 1');

-- With join statistics for the foreign key: the estimate is unchanged
CREATE STATISTICS join_mcv_fk_id_stats (mcv) ON i.y
FROM join_mcv_fk s JOIN join_mcv_fk_indexed i ON (s.id = i.id);
ANALYZE join_mcv_fk;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk s
      JOIN join_mcv_fk_indexed i ON (s.id = i.id AND s.b = i.b)
     WHERE i.y = 1');

DROP TABLE join_mcv_fk, join_mcv_fk_indexed;

-- check two foreign keys count as two conditions, and the error is in the
-- planner's estimate of the join itself, which no correction can repair
CREATE TABLE join_mcv_fk2_indexed (id int PRIMARY KEY, code int UNIQUE, y int)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_fk2 (id int REFERENCES join_mcv_fk2_indexed (id),
                           code int REFERENCES join_mcv_fk2_indexed (code))
    WITH (autovacuum_enabled = off);

-- code = id, with y = 1 in a few rows
INSERT INTO join_mcv_fk2_indexed
SELECT g, g, (g <= 10)::int FROM generate_series(1, 100) g;

-- half the rows reference the rows where y = 1, by both keys
INSERT INTO join_mcv_fk2
SELECT r, r
FROM (SELECT CASE WHEN g <= 500 THEN 1 + g % 10 ELSE 11 + g % 90 END
      FROM generate_series(1, 1000) g) AS s(r);

ANALYZE join_mcv_fk2, join_mcv_fk2_indexed;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk2 s
      JOIN join_mcv_fk2_indexed i ON (s.id = i.id AND s.code = i.code)
     WHERE i.y = 1');

-- With join statistics for each of the foreign keys: neither applies
CREATE STATISTICS join_mcv_fk2_id_stats (mcv) ON i.y
FROM join_mcv_fk2 s JOIN join_mcv_fk2_indexed i ON (s.id = i.id);
CREATE STATISTICS join_mcv_fk2_code_stats (mcv) ON i.y
FROM join_mcv_fk2 s JOIN join_mcv_fk2_indexed i ON (s.code = i.code);
ANALYZE join_mcv_fk2;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_fk2 s
      JOIN join_mcv_fk2_indexed i ON (s.id = i.id AND s.code = i.code)
     WHERE i.y = 1');

DROP TABLE join_mcv_fk2, join_mcv_fk2_indexed;

-- check semi and anti joins are not corrected, and EXISTS on a primary
-- key, which becomes an inner join, is
CREATE TABLE join_mcv_semi_indexed (id int PRIMARY KEY, b int)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_semi (dim_id int) WITH (autovacuum_enabled = off);

-- b = 1 in a few rows
INSERT INTO join_mcv_semi_indexed
SELECT i, (i <= 10)::int FROM generate_series(1, 100) i;

-- most rows join the rows where b = 1; the last rows are joined by none
INSERT INTO join_mcv_semi
SELECT CASE WHEN g <= 900 THEN 1 + g % 10 ELSE 11 + g % 50 END
FROM generate_series(1, 1000) g;

ANALYZE join_mcv_semi_indexed, join_mcv_semi;

-- Without join statistics
-- An inner join, for comparison.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
      JOIN join_mcv_semi_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 1');
-- Rows of join_mcv_semi with a match where b = 1, and without one.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
     WHERE EXISTS (SELECT 1 FROM join_mcv_semi_indexed dim
                    WHERE dim.id = f.dim_id AND dim.b = 1)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
     WHERE NOT EXISTS (SELECT 1 FROM join_mcv_semi_indexed dim
                        WHERE dim.id = f.dim_id AND dim.b = 1)');
-- Rows of join_mcv_semi_indexed with b = 0 that are joined, and that are not.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi_indexed dim
     WHERE dim.b = 0
       AND EXISTS (SELECT 1 FROM join_mcv_semi f
                    WHERE f.dim_id = dim.id)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi_indexed dim
     WHERE dim.b = 0
       AND NOT EXISTS (SELECT 1 FROM join_mcv_semi f
                        WHERE f.dim_id = dim.id)');

-- With join statistics that store every value of b in the join
CREATE STATISTICS join_mcv_semi_stats (mcv) ON dim.b
FROM join_mcv_semi f JOIN join_mcv_semi_indexed dim ON (f.dim_id = dim.id);
ANALYZE join_mcv_semi;
-- An inner join, for comparison.
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
      JOIN join_mcv_semi_indexed dim ON (f.dim_id = dim.id)
     WHERE dim.b = 1');
-- Rows of join_mcv_semi with a match where b = 1: EXISTS on the primary key
-- of join_mcv_semi_indexed becomes an inner join, which the statistics correct
-- as above
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
     WHERE EXISTS (SELECT 1 FROM join_mcv_semi_indexed dim
                    WHERE dim.id = f.dim_id AND dim.b = 1)');
-- and without one: an anti join keeps its estimate, as the statistics do not
-- record which rows have no match
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi f
     WHERE NOT EXISTS (SELECT 1 FROM join_mcv_semi_indexed dim
                        WHERE dim.id = f.dim_id AND dim.b = 1)');
-- Rows of join_mcv_semi_indexed with b = 0 that are joined, and that are not:
-- with join_mcv_semi_indexed on the outer side the statistics are not used
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi_indexed dim
     WHERE dim.b = 0
       AND EXISTS (SELECT 1 FROM join_mcv_semi f
                    WHERE f.dim_id = dim.id)');
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_semi_indexed dim
     WHERE dim.b = 0
       AND NOT EXISTS (SELECT 1 FROM join_mcv_semi f
                        WHERE f.dim_id = dim.id)');

DROP TABLE join_mcv_semi, join_mcv_semi_indexed;

-- check join statistics on the partitions are used in a partitionwise join
CREATE TABLE join_mcv_part (a int) PARTITION BY RANGE (a);
CREATE TABLE join_mcv_part_1 PARTITION OF join_mcv_part
    FOR VALUES FROM (0) TO (100) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_part_2 PARTITION OF join_mcv_part
    FOR VALUES FROM (100) TO (200) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_part_indexed (a int, y int) PARTITION BY RANGE (a);
CREATE TABLE join_mcv_part_indexed_1 PARTITION OF join_mcv_part_indexed
    FOR VALUES FROM (0) TO (100) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_part_indexed_2 PARTITION OF join_mcv_part_indexed
    FOR VALUES FROM (100) TO (200) WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_part_indexed (a);

-- y = 1 in a few rows of each partition
INSERT INTO join_mcv_part_indexed
SELECT g, (g % 100 < 10)::int FROM generate_series(0, 199) g;

-- most rows join the rows where y = 1
INSERT INTO join_mcv_part
SELECT CASE WHEN g % 10 < 9 THEN g % 10 ELSE g % 100 END + (g % 2) * 100
FROM generate_series(1, 2000) g;

ANALYZE join_mcv_part, join_mcv_part_indexed;

SET enable_partitionwise_join = on;
-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_part s
      JOIN join_mcv_part_indexed i ON (s.a = i.a)
     WHERE i.y = 1');

-- With join statistics on the partitions
CREATE STATISTICS join_mcv_part_1_stats (mcv) ON i.y
FROM join_mcv_part_1 s JOIN join_mcv_part_indexed_1 i ON (s.a = i.a);
CREATE STATISTICS join_mcv_part_2_stats (mcv) ON i.y
FROM join_mcv_part_2 s JOIN join_mcv_part_indexed_2 i ON (s.a = i.a);
ANALYZE join_mcv_part_1, join_mcv_part_2;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_part s
      JOIN join_mcv_part_indexed i ON (s.a = i.a)
     WHERE i.y = 1');
RESET enable_partitionwise_join;

DROP TABLE join_mcv_part, join_mcv_part_indexed;

-- check ANALYZE of the anchor table builds the statistics, from its sample
-- and the matching rows of the probed table, and ANALYZE of the probed
-- table does not
CREATE STATISTICS join_mcv_movies_probe_stats (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_keywords;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_movies_probe_stats';
ANALYZE join_mcv_movies;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_movies_probe_stats';
DROP STATISTICS join_mcv_movies_probe_stats;

-- check sampling a join value with many matches looks the value up once
-- through the index on the probed table, however many rows have it
CREATE TABLE join_mcv_dups (k int, a int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_dups_indexed (k int, b int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_dups_indexed (k);
-- Four rows with join value 1, one with 2, and one NULL
INSERT INTO join_mcv_dups
VALUES (1, 1), (1, 1), (1, 1), (1, 2), (2, 3), (NULL, 4);
-- many matches each for 1 and 2, and rows with 0 that no row joins
INSERT INTO join_mcv_dups_indexed SELECT 1, 3 FROM generate_series(1, 100);
INSERT INTO join_mcv_dups_indexed SELECT 2, 5 FROM generate_series(1, 100);
INSERT INTO join_mcv_dups_indexed SELECT 0, 1 FROM generate_series(1, 100);
ANALYZE join_mcv_dups, join_mcv_dups_indexed;
CREATE STATISTICS join_mcv_dups_stats (mcv) ON s.a, i.b
FROM join_mcv_dups s JOIN join_mcv_dups_indexed i ON (s.k = i.k);
ANALYZE join_mcv_dups;

-- one index scan per join value that is not NULL, each match read once
SELECT pg_stat_force_next_flush();
SELECT idx_scan, idx_tup_read, idx_tup_fetch FROM pg_stat_all_indexes
  WHERE indexrelid = 'join_mcv_dups_indexed_k_idx'::regclass;

-- each row with join value 1 pairs with every match of 1, the row with 2
-- with the matches of 2, and the NULL row with none
SELECT m.values, m.frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_dups_stats'
ORDER BY m.frequency DESC, m.values;

DROP TABLE join_mcv_dups, join_mcv_dups_indexed;

-- With a hash index on the probed table, a lookup of a join value also
-- returns the matches of every value with the same hash code, and ANALYZE
-- must leave them out.  2775 and 131913 have the same hash code.
SELECT hashint4(2775) = hashint4(131913) AS same_hash_code;
CREATE TABLE join_mcv_hash (k int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_hash_indexed (k int) WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_hash_indexed USING hash (k);
INSERT INTO join_mcv_hash VALUES (2775);
INSERT INTO join_mcv_hash_indexed
VALUES (2775), (2775), (2775), (2775), (131913);
ANALYZE join_mcv_hash, join_mcv_hash_indexed;
CREATE STATISTICS join_mcv_hash_stats (mcv) ON i.k
FROM join_mcv_hash s JOIN join_mcv_hash_indexed i ON (s.k = i.k);
ANALYZE join_mcv_hash;

-- only the matches of 2775
SELECT m.values, m.frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_hash_stats';

DROP TABLE join_mcv_hash, join_mcv_hash_indexed;

-- check a join larger than the sample: with statistics target 10, ANALYZE
-- samples 1000 of the 100000 joined rows, so the frequencies vary from run
-- to run
CREATE TABLE join_mcv_big (k int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_big_indexed (k int, b int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_big_indexed (k);
-- one join value, whose matches have b in 1..4 in equal shares
INSERT INTO join_mcv_big SELECT 1 FROM generate_series(1, 40);
INSERT INTO join_mcv_big_indexed
SELECT 1, (g - 1) / 625 + 1 FROM generate_series(1, 2500) g;
ANALYZE join_mcv_big, join_mcv_big_indexed;
CREATE STATISTICS join_mcv_big_stats (mcv) ON i.b
FROM join_mcv_big s JOIN join_mcv_big_indexed i ON (s.k = i.k);
ALTER STATISTICS join_mcv_big_stats SET STATISTICS 10;
ANALYZE join_mcv_big;

-- the sample has every value of b, not only the first ones, and exactly the
-- rows the target allows, so every frequency is a multiple of 1/1000
SELECT count(*) AS b_values,
       bool_and(abs(m.frequency * 1000 - round(m.frequency * 1000)) < 1e-6)
           AS in_thousandths
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_big_stats';

DROP TABLE join_mcv_big, join_mcv_big_indexed;

-- check ANALYZE samples as many rows as the target of the join statistics
-- needs, even when the columns of the table need fewer
CREATE TABLE join_mcv_target (k int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_target_indexed (k int PRIMARY KEY, v int)
    WITH (autovacuum_enabled = off);
-- v is 1 in half the rows of join_mcv_target_indexed, 2 in 30% and 3 in 20%,
-- and each of them joins ten of the 1000 rows of join_mcv_target
INSERT INTO join_mcv_target_indexed
SELECT g, CASE WHEN g <= 50 THEN 1 WHEN g <= 80 THEN 2 ELSE 3 END
FROM generate_series(1, 100) g;
INSERT INTO join_mcv_target SELECT (g - 1) / 10 + 1 FROM generate_series(1, 1000) g;
ANALYZE join_mcv_target_indexed;
-- the column k alone needs a sample of only 300 rows
ALTER TABLE join_mcv_target ALTER COLUMN k SET STATISTICS 1;
-- without a target of their own, the statistics take the largest target of
-- their columns, here the default target of v, which needs 30000 rows, so the
-- sample has all 1000 rows and the frequencies are exact
CREATE STATISTICS join_mcv_target_stats (mcv) ON i.v
FROM join_mcv_target t JOIN join_mcv_target_indexed i ON (t.k = i.k);
ANALYZE join_mcv_target;
SELECT m.values, m.frequency
FROM pg_statistic_ext s
JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
CROSS JOIN LATERAL pg_mcv_list_items(d.stxdmcv) AS m
WHERE s.stxname = 'join_mcv_target_stats'
ORDER BY m.frequency DESC;
-- the statistics are skipped when one of their columns is not analyzed, as
-- for statistics on a single table
ALTER TABLE join_mcv_target_indexed ALTER COLUMN v SET STATISTICS 0;
ANALYZE join_mcv_target;
DROP TABLE join_mcv_target, join_mcv_target_indexed;

-- check the join column of the probed table needs an index ANALYZE can look
-- the join values up in: without one, CREATE warns and ANALYZE skips the
-- object
-- XXX Sampling without an index, by scanning the probed table, is planned;
-- the cases skipped here will then build.
CREATE TABLE join_mcv_noidx (id INTEGER, val TEXT);
-- a few rows, joined by the movies with keyword_id 1
INSERT INTO join_mcv_noidx SELECT 1, 'x' FROM generate_series(1, 20);
CREATE STATISTICS join_mcv_noidx_stats (mcv) ON n.val
FROM join_mcv_movies m JOIN join_mcv_noidx n ON (m.keyword_id = n.id);
ANALYZE join_mcv_movies;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_noidx_stats';
-- None of these is suitable: a BRIN index does not support equality lookup,
-- a partial index does not cover every row, and a multi-column index can be
-- looked up only by its leading column.
CREATE INDEX join_mcv_noidx_brin ON join_mcv_noidx USING brin (id);
CREATE INDEX join_mcv_noidx_partial ON join_mcv_noidx (id) WHERE id > 0;
CREATE INDEX join_mcv_noidx_nonlead ON join_mcv_noidx (val, id);
ANALYZE join_mcv_movies;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_noidx_stats';
-- A btree index with the join column leading is suitable.
CREATE INDEX join_mcv_noidx_wide ON join_mcv_noidx (id, val);
ANALYZE join_mcv_movies;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_noidx_stats';
DROP INDEX join_mcv_noidx_wide;
-- With the last usable index gone, ANALYZE warns and keeps the data it has
ANALYZE join_mcv_movies;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_noidx_stats';
DROP TABLE join_mcv_noidx;

-- check ALTER TABLE ... ALTER COLUMN TYPE re-creates the statistics objects
-- that use the column, as for single-table statistics, and leaves the
-- others alone
CREATE STATISTICS join_mcv_movies_alter_stats (mcv) ON mk.year
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;

ALTER TABLE join_mcv_keywords ALTER COLUMN phonetic_code TYPE varchar(10);

SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_movies_alter_stats';

-- A column of the statistics: re-created without data
ALTER TABLE join_mcv_movies ALTER COLUMN year TYPE numeric;

SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_movies_alter_stats';

DROP STATISTICS join_mcv_movies_alter_stats;

-- Changing the type of a column of the probed table re-creates the
-- statistics, as for a column of the table they belong to
CREATE STATISTICS join_mcv_movies_alter_dim_stats (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ANALYZE join_mcv_movies;
SELECT pg_get_statisticsobjdef_body(s.oid), d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s LEFT JOIN pg_statistic_ext_data d ON d.stxoid = s.oid
WHERE s.stxname = 'join_mcv_movies_alter_dim_stats';
BEGIN;
ALTER TABLE join_mcv_keywords ALTER COLUMN keyword TYPE varchar(20);
-- ALTER TABLE does not lock join_mcv_movies exclusively: it is locked only
-- as for CREATE STATISTICS
SELECT mode FROM pg_locks
WHERE pid = pg_backend_pid() AND relation = 'join_mcv_movies'::regclass
ORDER BY mode;
COMMIT;
SELECT pg_get_statisticsobjdef_body(s.oid), d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s LEFT JOIN pg_statistic_ext_data d ON d.stxoid = s.oid
WHERE s.stxname = 'join_mcv_movies_alter_dim_stats';
DROP STATISTICS join_mcv_movies_alter_dim_stats;

-- A self-join's statistics are re-created once, although both sides change
CREATE STATISTICS join_mcv_movies_alter_self_stats (mcv) ON b.keyword
FROM join_mcv_keywords a JOIN join_mcv_keywords b ON (a.id = b.id);
ALTER TABLE join_mcv_keywords ALTER COLUMN id TYPE bigint;
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_movies_alter_self_stats';
DROP STATISTICS join_mcv_movies_alter_self_stats;

-- A join column may change to a type that can still be compared with the
-- other one (join_mcv_keywords.id is bigint since the case above); otherwise
-- ALTER TABLE fails, as the statistics cannot be re-created
CREATE STATISTICS join_mcv_movies_alter_key_stats (mcv) ON k.keyword
FROM join_mcv_movies mk JOIN join_mcv_keywords k ON (mk.keyword_id = k.id);
ALTER TABLE join_mcv_movies ALTER COLUMN keyword_id TYPE bigint;
ALTER TABLE join_mcv_movies ALTER COLUMN keyword_id TYPE numeric;
DROP STATISTICS join_mcv_movies_alter_key_stats;

DROP TABLE join_mcv_movies, join_mcv_keywords;

-- check tables with inheritance children are refused like partitioned
-- tables, whichever table has them, and children added later make ANALYZE
-- skip the statistics and the planner not use the data built before
CREATE TABLE join_mcv_inh (dim_id int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_inh_indexed (id int PRIMARY KEY, label text)
    WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_inh_indexed
SELECT g, 'label ' || g % 3 FROM generate_series(1, 99) g;
INSERT INTO join_mcv_inh SELECT 1 + g % 99 FROM generate_series(1, 1000) g;
CREATE TABLE join_mcv_inh_child () INHERITS (join_mcv_inh);
CREATE STATISTICS join_mcv_inh_anchor_stats (mcv) ON d.label
FROM join_mcv_inh f JOIN join_mcv_inh_indexed d ON (f.dim_id = d.id);
DROP TABLE join_mcv_inh_child;
CREATE TABLE join_mcv_inh_indexed_child () INHERITS (join_mcv_inh_indexed);
CREATE STATISTICS join_mcv_inh_probed_stats (mcv) ON d.label
FROM join_mcv_inh f JOIN join_mcv_inh_indexed d ON (f.dim_id = d.id);
DROP TABLE join_mcv_inh_indexed_child;

-- Children added later: the statistics are skipped, rather than built
-- without the children's rows, and the data built before are kept
CREATE STATISTICS join_mcv_inh_later_stats (mcv) ON d.label
FROM join_mcv_inh f JOIN join_mcv_inh_indexed d ON (f.dim_id = d.id);
ANALYZE join_mcv_inh;
CREATE TABLE join_mcv_inh_indexed_child () INHERITS (join_mcv_inh_indexed)
    WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_inh_indexed_child
SELECT g, 'child' FROM generate_series(1, 99) g;
ANALYZE join_mcv_inh, join_mcv_inh_indexed, join_mcv_inh_indexed_child;
SELECT most_common_vals FROM pg_stats_ext
WHERE statistics_name = 'join_mcv_inh_later_stats';
-- The data describe the join without the children's rows, so they are not
-- used for a join with them
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_inh f JOIN join_mcv_inh_indexed d
        ON (f.dim_id = d.id)
    WHERE d.label = ''label 1''
');
-- but are for the join they describe
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_inh f JOIN ONLY join_mcv_inh_indexed d
        ON (f.dim_id = d.id)
    WHERE d.label = ''label 1''
');
-- Children of the anchor table are reported once, although ANALYZE goes over
-- that table both with and without its children
CREATE TABLE join_mcv_inh_child () INHERITS (join_mcv_inh);
ANALYZE join_mcv_inh;
DROP TABLE join_mcv_inh_child;
-- Without the statistics, the join with the children is estimated as above
DROP STATISTICS join_mcv_inh_later_stats;
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_inh f JOIN join_mcv_inh_indexed d
        ON (f.dim_id = d.id)
    WHERE d.label = ''label 1''
');

DROP TABLE join_mcv_inh, join_mcv_inh_indexed CASCADE;

-- Permission tests.  CREATE requires ownership of the anchor table and
-- SELECT on the probed table.  ANALYZE refreshes the statistics only if the
-- owner still has SELECT on the probed table; otherwise it warns and skips
-- them, leaving the existing data in place.
--
CREATE ROLE regress_join_mcv_user;
CREATE ROLE regress_join_mcv_user2;

CREATE TABLE join_mcv_perm (id int, val int) WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_perm_indexed (id int PRIMARY KEY, label text)
    WITH (autovacuum_enabled = off);
INSERT INTO join_mcv_perm SELECT i, i % 10 FROM generate_series(1, 1000) i;
INSERT INTO join_mcv_perm_indexed SELECT i, 'l' FROM generate_series(1, 1000) i;
ANALYZE join_mcv_perm, join_mcv_perm_indexed;

GRANT CREATE ON SCHEMA public TO regress_join_mcv_user, regress_join_mcv_user2;
GRANT SELECT ON join_mcv_perm, join_mcv_perm_indexed TO regress_join_mcv_user;

-- Not the anchor owner: CREATE fails
SET SESSION AUTHORIZATION regress_join_mcv_user;
CREATE STATISTICS join_mcv_perm_stats (mcv) ON a.val
FROM join_mcv_perm a JOIN join_mcv_perm_indexed i ON (a.id = i.id);
RESET SESSION AUTHORIZATION;
ALTER TABLE join_mcv_perm OWNER TO regress_join_mcv_user;

-- Anchor owner, no SELECT on the probed table: CREATE fails
REVOKE SELECT ON join_mcv_perm_indexed FROM regress_join_mcv_user;
SET SESSION AUTHORIZATION regress_join_mcv_user;
CREATE STATISTICS join_mcv_perm_stats (mcv) ON a.val
FROM join_mcv_perm a JOIN join_mcv_perm_indexed i ON (a.id = i.id);
RESET SESSION AUTHORIZATION;

-- Anchor owner, SELECT on the probed table: CREATE succeeds
GRANT SELECT ON join_mcv_perm_indexed TO regress_join_mcv_user;
SET SESSION AUTHORIZATION regress_join_mcv_user;
CREATE STATISTICS join_mcv_perm_stats (mcv) ON a.val
FROM join_mcv_perm a JOIN join_mcv_perm_indexed i ON (a.id = i.id);
RESET SESSION AUTHORIZATION;

-- Owner has SELECT: ANALYZE builds the statistics
ANALYZE join_mcv_perm;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_perm_stats';

-- Anchor owner changed: ANALYZE still refreshes (no warning), and ALTER
-- COLUMN TYPE still re-creates the object for its owner, without data until
-- the next ANALYZE
ALTER TABLE join_mcv_perm OWNER TO CURRENT_USER;
ANALYZE join_mcv_perm;
ALTER TABLE join_mcv_perm ALTER COLUMN val TYPE bigint;
SELECT stxname, stxowner::regrole FROM pg_statistic_ext
WHERE stxname = 'join_mcv_perm_stats';
ANALYZE join_mcv_perm;

-- Statistics owner changed to a role without SELECT on the probed table:
-- ANALYZE skips
ALTER STATISTICS join_mcv_perm_stats OWNER TO regress_join_mcv_user2;
ANALYZE join_mcv_perm;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_perm_stats';

-- Owner granted SELECT on the probed table: ANALYZE refreshes (no warning)
GRANT SELECT ON join_mcv_perm_indexed TO regress_join_mcv_user2;
ANALYZE join_mcv_perm;

-- Owner's SELECT on the probed table revoked: ANALYZE skips, while ALTER
-- COLUMN TYPE still re-creates the object (no error)
REVOKE SELECT ON join_mcv_perm_indexed FROM regress_join_mcv_user2;
ANALYZE join_mcv_perm;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_perm_stats';
ALTER TABLE join_mcv_perm ALTER COLUMN val TYPE int;

DROP STATISTICS join_mcv_perm_stats;
DROP TABLE join_mcv_perm, join_mcv_perm_indexed;
REVOKE CREATE ON SCHEMA public
    FROM regress_join_mcv_user, regress_join_mcv_user2;
DROP ROLE regress_join_mcv_user, regress_join_mcv_user2;

-- check the owner of a joined table can drop their index, Tomas Vondra's
-- scenario: owner1 owns the anchor and the statistics object, owner2 the
-- other table and its index
CREATE ROLE regress_join_mcv_owner1;
CREATE ROLE regress_join_mcv_owner2;
GRANT USAGE, CREATE ON SCHEMA public TO regress_join_mcv_owner1;
GRANT USAGE, CREATE ON SCHEMA public TO regress_join_mcv_owner2;
SET ROLE regress_join_mcv_owner2;
CREATE TABLE join_mcv_owners_indexed (a int, b int);
INSERT INTO join_mcv_owners_indexed
SELECT g, g % 2 FROM generate_series(1, 100) g;
CREATE INDEX join_mcv_owners_idx ON join_mcv_owners_indexed (a);
RESET ROLE;
GRANT SELECT ON join_mcv_owners_indexed TO regress_join_mcv_owner1;
SET ROLE regress_join_mcv_owner1;
CREATE TABLE join_mcv_owners (a int, b int);
INSERT INTO join_mcv_owners
SELECT g % 100 + 1, g % 3 FROM generate_series(1, 1000) g;
CREATE STATISTICS join_mcv_owners_stats (mcv) ON s.a, s.b, i.a, i.b
  FROM join_mcv_owners s JOIN join_mcv_owners_indexed i ON (s.a = i.a);
ANALYZE join_mcv_owners;
-- one dependency row per column, although s.a and i.a are both join and
-- statistics columns
SELECT pg_describe_object(refclassid, refobjid, refobjsubid) AS ref
FROM pg_depend
WHERE objid = (SELECT oid FROM pg_statistic_ext
               WHERE stxname = 'join_mcv_owners_stats')
  AND refobjsubid <> 0
ORDER BY 1;
SET ROLE regress_join_mcv_owner2;
DROP INDEX join_mcv_owners_idx;
-- The statistics object stays, and is skipped until owner2 has an index
-- again
SET ROLE regress_join_mcv_owner1;
ANALYZE join_mcv_owners;
SELECT stxname FROM pg_statistic_ext WHERE stxname = 'join_mcv_owners_stats';
SET ROLE regress_join_mcv_owner2;
CREATE INDEX join_mcv_owners_idx ON join_mcv_owners_indexed (a);
SET ROLE regress_join_mcv_owner1;
ANALYZE join_mcv_owners;
-- owner1 sees the data in pg_stats_ext, as they may build it with only SELECT
-- on join_mcv_owners_indexed
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_name = 'join_mcv_owners_stats';
-- but owner2 does not, not owning join_mcv_owners
SET ROLE regress_join_mcv_owner2;
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_name = 'join_mcv_owners_stats';
-- nor owner1 once they lose SELECT on join_mcv_owners_indexed, although the
-- data stays
RESET ROLE;
REVOKE SELECT ON join_mcv_owners_indexed FROM regress_join_mcv_owner1;
SET ROLE regress_join_mcv_owner1;
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_name = 'join_mcv_owners_stats';
RESET ROLE;
SELECT d.stxdmcv IS NOT NULL AS has_mcv
FROM pg_statistic_ext s
LEFT JOIN pg_statistic_ext_data d ON (s.oid = d.stxoid)
WHERE s.stxname = 'join_mcv_owners_stats';
DROP TABLE join_mcv_owners, join_mcv_owners_indexed;
REVOKE USAGE, CREATE ON SCHEMA public
    FROM regress_join_mcv_owner1, regress_join_mcv_owner2;
DROP ROLE regress_join_mcv_owner1, regress_join_mcv_owner2;

-- check join statistics do not reveal rows that row-level security hides
CREATE FUNCTION op_leak(int, int) RETURNS bool
    AS 'BEGIN RAISE NOTICE ''op_leak => %, %'', $1, $2; RETURN $1 < $2; END'
    LANGUAGE plpgsql;
-- The join MCV estimator only considers operators that have a restriction
-- estimator and belong to a btree operator family.
CREATE OPERATOR <<< (procedure = op_leak, leftarg = int, rightarg = int,
                     restrict = scalarltsel);
CREATE OPERATOR FAMILY leak_ops USING btree;
ALTER OPERATOR FAMILY leak_ops USING btree ADD OPERATOR 1 <<< (int4, int4);

CREATE ROLE regress_join_mcv_rls_owner;
CREATE ROLE regress_join_mcv_rls_anchor_owner;
CREATE SCHEMA join_rls AUTHORIZATION regress_join_mcv_rls_owner;
SET ROLE regress_join_mcv_rls_owner;
CREATE TABLE join_rls.anchor (id int, filler int);
CREATE TABLE join_rls.secret (id int, secret text);
INSERT INTO join_rls.anchor SELECT g, g FROM generate_series(1, 100) g;
INSERT INTO join_rls.secret
    SELECT g, CASE WHEN g <= 50 THEN 'secret-1' ELSE 'secret-2' END
    FROM generate_series(1, 100) g;
CREATE INDEX ON join_rls.secret (id);
CREATE STATISTICS join_rls.secret_stats (mcv) ON anchor.filler, secret.secret
    FROM join_rls.anchor JOIN join_rls.secret ON anchor.id = secret.id;
ANALYZE join_rls.anchor;
-- No column statistics on filler, so op_leak can only see the join MCV's
-- values
SELECT pg_clear_attribute_stats('join_rls', 'anchor', 'filler', false);

-- The owner can read secret, so it sees the statistics.
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_schemaname = 'join_rls';

-- Owning only anchor is not enough to see them
RESET ROLE;
ALTER TABLE join_rls.anchor OWNER TO regress_join_mcv_rls_anchor_owner;
SET ROLE regress_join_mcv_rls_anchor_owner;
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_schemaname = 'join_rls';
RESET ROLE;
ALTER TABLE join_rls.anchor OWNER TO regress_join_mcv_rls_owner;
SET ROLE regress_join_mcv_rls_owner;

-- Now nobody can read any rows of secret, not even its owner.  (With no
-- policy at all, the planner would prove the join empty and never estimate
-- it.)
ALTER TABLE join_rls.secret ENABLE ROW LEVEL SECURITY;
ALTER TABLE join_rls.secret FORCE ROW LEVEL SECURITY;
CREATE POLICY nobody ON join_rls.secret USING (id < 0);
SELECT count(*) AS visible_secret_rows FROM join_rls.secret;

-- So its values must not show up in pg_stats_ext,
SELECT statistics_name, most_common_vals IS NOT NULL AS has_mcv
FROM pg_stats_ext WHERE statistics_schemaname = 'join_rls';

-- nor be passed to a leaky operator while planning: no op_leak NOTICE.
EXPLAIN (COSTS OFF)
SELECT * FROM join_rls.anchor JOIN join_rls.secret ON anchor.id = secret.id
    WHERE anchor.filler <<< 5;
RESET ROLE;

DROP SCHEMA join_rls CASCADE;
DROP ROLE regress_join_mcv_rls_owner, regress_join_mcv_rls_anchor_owner;
DROP OPERATOR FAMILY leak_ops USING btree;
DROP OPERATOR <<< (int4, int4);
DROP FUNCTION op_leak(int, int);

-- These objects are not dropped, so that the pg_upgrade test dumps and
-- restores join statistics of each of these shapes; the same holds for
-- join_mcv_stored and join_mcv_filters above.
--
-- check a cross-type join, a float4 anchor column joined to a float8 column
-- through the cross-type equality operator
CREATE TABLE join_mcv_crosstype_indexed (id float8 PRIMARY KEY,
                                         color text NOT NULL)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_crosstype (id serial, dim_id float4 NOT NULL)
    WITH (autovacuum_enabled = off);

-- three colors
INSERT INTO join_mcv_crosstype_indexed
SELECT i::float8, CASE WHEN i <= 5 THEN 'red'
                       WHEN i <= 10 THEN 'blue'
                       ELSE 'green' END
FROM generate_series(1, 15) i;

-- most rows reference the red rows
INSERT INTO join_mcv_crosstype (dim_id)
SELECT CASE WHEN i % 5 < 4 THEN (i % 5 + 1)::float4
            ELSE (6 + i % 10)::float4 END
FROM generate_series(1, 500) i;

ANALYZE join_mcv_crosstype_indexed;
ANALYZE join_mcv_crosstype;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_crosstype f, join_mcv_crosstype_indexed d
    WHERE f.dim_id = d.id AND d.color = ''red''
');

CREATE STATISTICS join_mcv_crosstype_stats (mcv) ON f.dim_id, d.color
FROM join_mcv_crosstype f
     JOIN join_mcv_crosstype_indexed d ON (f.dim_id = d.id);

ANALYZE join_mcv_crosstype;

-- With join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_crosstype f, join_mcv_crosstype_indexed d
    WHERE f.dim_id = d.id AND d.color = ''red''
');

-- The join condition may name the joined table first: the statistics are the
-- same
CREATE STATISTICS join_mcv_crosstype_swapped_stats (mcv) ON f.dim_id, d.color
FROM join_mcv_crosstype f
     JOIN join_mcv_crosstype_indexed d ON (d.id = f.dim_id);
ANALYZE join_mcv_crosstype;
SELECT a.most_common_vals = b.most_common_vals AS same_vals,
       a.most_common_freqs = b.most_common_freqs AS same_freqs
FROM pg_stats_ext a, pg_stats_ext b
WHERE a.statistics_name = 'join_mcv_crosstype_stats'
  AND b.statistics_name = 'join_mcv_crosstype_swapped_stats';
DROP STATISTICS join_mcv_crosstype_swapped_stats;

-- Leave join_mcv_crosstype, join_mcv_crosstype_indexed and
-- join_mcv_crosstype_stats in place for pg_upgrade testing.

-- check a join of binary-compatible types, a varchar column joined to a
-- text key: the parser relabels the varchar column to text, as the planner
-- does for the same join in a query
CREATE TABLE join_mcv_relabel_indexed (code text PRIMARY KEY,
                                       color text NOT NULL)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_relabel (id serial, dim_code varchar(10) NOT NULL)
    WITH (autovacuum_enabled = off);

-- three colors
INSERT INTO join_mcv_relabel_indexed
SELECT 'c' || i, CASE WHEN i <= 5 THEN 'red'
                      WHEN i <= 10 THEN 'blue'
                      ELSE 'green' END
FROM generate_series(1, 15) i;

-- most rows reference the red rows
INSERT INTO join_mcv_relabel (dim_code)
SELECT 'c' || CASE WHEN i % 5 < 4 THEN i % 5 + 1 ELSE 6 + i % 10 END
FROM generate_series(1, 500) i;

ANALYZE join_mcv_relabel_indexed;
ANALYZE join_mcv_relabel;

-- Without join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_relabel f, join_mcv_relabel_indexed d
    WHERE f.dim_code = d.code AND d.color = ''red''
');

CREATE STATISTICS join_mcv_relabel_stats (mcv) ON d.color
FROM join_mcv_relabel f
     JOIN join_mcv_relabel_indexed d ON (f.dim_code = d.code);
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_relabel_stats';
-- Dependencies are recorded on both join columns, the varchar one included
SELECT pg_describe_object(refclassid, refobjid, refobjsubid) AS ref
FROM pg_depend
WHERE objid = (SELECT oid FROM pg_statistic_ext
               WHERE stxname = 'join_mcv_relabel_stats')
  AND refobjsubid <> 0
ORDER BY 1;
-- The join condition is stored with its collation
SELECT substring(stxjoinconds from ':inputcollid \d+') AS inputcollid
FROM pg_statistic_ext WHERE stxname = 'join_mcv_relabel_stats';
-- A column merged by USING can be used by a later join: it stands for the
-- varchar column
CREATE TABLE join_mcv_relabel_code (code varchar(10));
CREATE STATISTICS join_mcv_relabel_using_stats (mcv) ON d2.color
FROM join_mcv_relabel_code JOIN join_mcv_relabel_indexed USING (code)
     JOIN join_mcv_relabel_indexed d2 USING (code);
SELECT pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_relabel_using_stats';
DROP TABLE join_mcv_relabel_code;

ANALYZE join_mcv_relabel;

-- With join statistics
SELECT * FROM check_estimated_rows('
    SELECT * FROM join_mcv_relabel f, join_mcv_relabel_indexed d
    WHERE f.dim_code = d.code AND d.color = ''red''
');

-- LIKE INCLUDING STATISTICS copies only the single-table statistics: join
-- statistics describe a join, not the table
CREATE STATISTICS join_mcv_relabel_single_stats ON id, dim_code
FROM join_mcv_relabel;
CREATE TABLE join_mcv_relabel_copy
    (LIKE join_mcv_relabel INCLUDING STATISTICS);
SELECT stxname, pg_get_statisticsobjdef_body(oid) FROM pg_statistic_ext
WHERE stxrelid = 'join_mcv_relabel_copy'::regclass;
DROP TABLE join_mcv_relabel_copy;
DROP STATISTICS join_mcv_relabel_single_stats;

-- Leave join_mcv_relabel, join_mcv_relabel_indexed and join_mcv_relabel_stats
-- in place for pg_upgrade testing.

-- check same-named tables in different schemas get distinct aliases in the
-- deparsed definition

CREATE SCHEMA join_mcv_schema1;
CREATE SCHEMA join_mcv_schema2;

CREATE TABLE join_mcv_schema1.t (a int PRIMARY KEY)
    WITH (autovacuum_enabled = off);
CREATE TABLE join_mcv_schema2.t (b int)
    WITH (autovacuum_enabled = off);
CREATE INDEX ON join_mcv_schema2.t (b);

INSERT INTO join_mcv_schema1.t SELECT generate_series(1, 100);
INSERT INTO join_mcv_schema2.t SELECT generate_series(1, 100);

ANALYZE join_mcv_schema1.t;
ANALYZE join_mcv_schema2.t;

CREATE STATISTICS join_mcv_schemas_stats (mcv) ON join_mcv_schema2.t.b
FROM join_mcv_schema1.t
     JOIN join_mcv_schema2.t ON (join_mcv_schema1.t.a = join_mcv_schema2.t.b);

SELECT pg_get_statisticsobjdef(oid) FROM pg_statistic_ext
WHERE stxname = 'join_mcv_schemas_stats';
-- pg_stats_ext names each column as the definition does
ANALYZE join_mcv_schema1.t;
SELECT exprs FROM pg_stats_ext
WHERE statistics_name = 'join_mcv_schemas_stats';

-- Leave the join_mcv_schema1 and join_mcv_schema2 schemas and
-- join_mcv_schemas_stats in place for pg_upgrade testing.

-- check the auto-generated name of a join statistics object qualifies
-- columns with their relation, written before the first column and again
-- whenever the relation changes (single-table naming is covered in
-- stats_ext)
CREATE TABLE join_mcv_auto1 (a INTEGER, b INTEGER, c INTEGER);
CREATE TABLE join_mcv_auto2 (d INTEGER, e INTEGER, f INTEGER);
CREATE INDEX ON join_mcv_auto1 (a);
CREATE INDEX ON join_mcv_auto2 (d);

-- Two columns from each relation: each relation name is written once.
CREATE STATISTICS (mcv) ON a1.b, a1.c, a2.e, a2.f
    FROM join_mcv_auto1 a1 JOIN join_mcv_auto2 a2 ON (a1.a = a2.d);

-- Columns alternating between the relations: the relation name is written
-- again each time it changes.
CREATE STATISTICS (mcv) ON a2.e, a1.b, a2.f
    FROM join_mcv_auto1 a1 JOIN join_mcv_auto2 a2 ON (a1.a = a2.d);

-- Columns from only one of the relations: that relation name is written once.
CREATE STATISTICS (mcv) ON a2.e, a2.f
    FROM join_mcv_auto1 a1 JOIN join_mcv_auto2 a2 ON (a1.a = a2.d);

\dX join_mcv_auto*

-- Leave join_mcv_auto1, join_mcv_auto2 and their statistics in place for
-- pg_upgrade testing.

-- check definitions printed differently than written: tables in another
-- order, and several join conditions
CREATE TABLE join_mcv_shape (id INTEGER PRIMARY KEY, dim1_id INTEGER);
CREATE TABLE join_mcv_shape_dim1 (id INTEGER PRIMARY KEY, val TEXT);
CREATE TABLE join_mcv_shape_dim2 (id INTEGER PRIMARY KEY, label TEXT);
CREATE STATISTICS join_mcv_shape_nested_stats (mcv) ON d1.val, d2.label
    FROM join_mcv_shape f
    JOIN (join_mcv_shape_dim1 d1 JOIN join_mcv_shape_dim2 d2 ON (d1.id = d2.id))
    ON (f.dim1_id = d2.id);
CREATE STATISTICS join_mcv_shape_multiclause_stats (mcv) ON d1.val
    FROM join_mcv_shape f
    JOIN join_mcv_shape_dim1 d1 ON (f.dim1_id = d1.id AND f.id = d1.id);
SELECT stxname, stxjoinrels::regclass[], pg_get_statisticsobjdef_body(oid)
FROM pg_statistic_ext WHERE stxname LIKE 'join_mcv_shape%' ORDER BY stxname;
-- Leave join_mcv_shape, join_mcv_shape_dim1, join_mcv_shape_dim2 and their
-- statistics in place for pg_upgrade testing.
