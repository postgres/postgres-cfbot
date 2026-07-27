-- tests to validate bloom filter pushdown
-- no parallel query support for now
SET max_parallel_workers_per_gather = 0;

-- needed, because the filter pushdown eliminates one of the inputs for
-- choosing join order (reduction of result size)
SET join_collapse_limit = 1;

-- simple starjoin queries (fact + up to 7 dimensions)
CREATE TABLE bloom_star_dim_1 (id int, r real);
CREATE TABLE bloom_star_dim_2 (id int, r real);
CREATE TABLE bloom_star_dim_3 (id int, r real);
CREATE TABLE bloom_star_dim_4 (id int, r real);
CREATE TABLE bloom_star_dim_5 (id int, r real);
CREATE TABLE bloom_star_dim_6 (id int, r real);
CREATE TABLE bloom_star_dim_7 (id int, r real);

CREATE TABLE bloom_star_fact (id1 int, id2 int, id3 int, id4 int, id5 int, id6 int, id7 int, padding text);

INSERT INTO bloom_star_dim_1 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_2 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_3 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_4 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_5 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_6 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_star_dim_7 SELECT i, random() FROM generate_series(1,10000) s(i);

INSERT INTO bloom_star_fact
SELECT
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    md5(i::text)
FROM generate_series(1,1000000) s(i);

VACUUM ANALYZE;

-- simple query, no join is selective enough for a filter
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id);

-- last join is 75% selective (not enough for a filter to be created)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d7.r < 0.75;

-- last join is 50% selective (good enough for a filter push down)
-- the join will be executed done last
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d7.r < 0.5;

-- first join is 50% selective (good enough for a filter push down)
-- the join will be executed done last
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d1.r < 0.5;

-- two joins 50% selective
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d1.r < 0.4 AND d7.r < 0.5;

-- all joins selective for a filter (more than how many we allow)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d1.r < 0.3 AND d2.r < 0.35 AND d3.r < 0.4 AND d4.r < 0.45 AND d5.r < 0.5 AND d6.r < 0.55 AND d7.r < 0.6;


SET bloom_filter_pushdown_max = 10;

-- all joins selective for a filter
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_star_fact f
JOIN bloom_star_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_star_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_star_dim_3 d3 ON (f.id3 = d3.id)
JOIN bloom_star_dim_4 d4 ON (f.id4 = d4.id)
JOIN bloom_star_dim_5 d5 ON (f.id5 = d5.id)
JOIN bloom_star_dim_6 d6 ON (f.id6 = d6.id)
JOIN bloom_star_dim_7 d7 ON (f.id7 = d7.id)
WHERE d1.r < 0.3 AND d2.r < 0.35 AND d3.r < 0.4 AND d4.r < 0.45 AND d5.r < 0.5 AND d6.r < 0.55 AND d7.r < 0.6;

RESET bloom_filter_pushdown_max;
RESET join_collapse_limit;

DROP TABLE bloom_star_dim_1;
DROP TABLE bloom_star_dim_2;
DROP TABLE bloom_star_dim_3;
DROP TABLE bloom_star_dim_4;
DROP TABLE bloom_star_dim_5;
DROP TABLE bloom_star_dim_6;
DROP TABLE bloom_star_dim_7;
DROP TABLE bloom_star_fact;
