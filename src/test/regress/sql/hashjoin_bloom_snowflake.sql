-- tests to validate bloom filter pushdown
-- no parallel query support for now
SET max_parallel_workers_per_gather = 0;

-- simple snowflake queries (fact + up to 7 dimensions)
CREATE TABLE bloom_snowflake_dim_1_1 (id int, r real);
CREATE TABLE bloom_snowflake_dim_1_2 (id int, r real);
CREATE TABLE bloom_snowflake_dim_1 (id int, id11 int, id12 int, r real);
CREATE TABLE bloom_snowflake_dim_2_1 (id int, r real);
CREATE TABLE bloom_snowflake_dim_2_2 (id int, r real);
CREATE TABLE bloom_snowflake_dim_2 (id int, id21 int, id22 int, r real);

CREATE TABLE bloom_snowflake_fact (id1 int, id2 int, padding text);

INSERT INTO bloom_snowflake_dim_1_1 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_snowflake_dim_1_2 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_snowflake_dim_1 SELECT i, 1 + mod(i, 10000), 1 + mod(i, 10000), random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_snowflake_dim_2_1 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_snowflake_dim_2_2 SELECT i, random() FROM generate_series(1,10000) s(i);
INSERT INTO bloom_snowflake_dim_2 SELECT i, 1 + mod(i, 10000), 1 + mod(i, 10000), random() FROM generate_series(1,10000) s(i);

INSERT INTO bloom_snowflake_fact
SELECT
    1 + mod(i, 10000),
    1 + mod(i, 10000),
    md5(i::text)
FROM generate_series(1,1000000) s(i);

VACUUM ANALYZE;

-- simple query, no join is selective enough for a filter
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id);

-- join is 75% selective (not enough for a filter to be created)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d1.r < 0.75;

EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d11.r < 0.75;

-- join is 50% selective (good enough for pushdown)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d1.r < 0.5;

EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d11.r < 0.5;

-- increase the accepted build size (includes the fact)
SET bloom_filter_pushdown_max_build_relids = 4;

-- join is 50% selective (good enough for pushdown)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d1.r < 0.5;

EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d11.r < 0.5;

-- needed to stabilize the join order
SET join_collapse_limit = 1;

-- two joins with 75% selectivity (not enough for pushdown individually,
-- but good enough when combined)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_snowflake_fact f
JOIN bloom_snowflake_dim_1 d1 ON (f.id1 = d1.id)
JOIN bloom_snowflake_dim_1_1 d11 ON (d1.id11 = d11.id)
JOIN bloom_snowflake_dim_1_2 d12 ON (d1.id12 = d12.id)
JOIN bloom_snowflake_dim_2 d2 ON (f.id2 = d2.id)
JOIN bloom_snowflake_dim_2_1 d21 ON (d2.id21 = d21.id)
JOIN bloom_snowflake_dim_2_2 d22 ON (d2.id22 = d22.id)
WHERE d11.r < 0.75 AND d12.r < 0.75;

RESET join_collapse_limit;
RESET bloom_filter_pushdown_max_build_relids;

DROP TABLE bloom_snowflake_dim_1_1;
DROP TABLE bloom_snowflake_dim_1_2;
DROP TABLE bloom_snowflake_dim_1;
DROP TABLE bloom_snowflake_dim_2_1;
DROP TABLE bloom_snowflake_dim_2_2;
DROP TABLE bloom_snowflake_dim_2;
DROP TABLE bloom_snowflake_fact;
