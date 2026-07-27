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



-- snowflake queries on multi-column FK joins
CREATE TABLE bloom_snowflake_multi_dim_1_1 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_snowflake_multi_dim_1_2 (a int, b int, r real, primary key (a, b));

CREATE TABLE bloom_snowflake_multi_dim_1 (
	a int, b int,
	id11a int, id11b int,
	id12a int, id12b int,
	r real,
	foreign key (id11a, id11b) references bloom_snowflake_multi_dim_1_1(a, b),
	foreign key (id12a, id12b) references bloom_snowflake_multi_dim_1_2(a, b),
	primary key (a, b));

CREATE TABLE bloom_snowflake_multi_dim_2_1 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_snowflake_multi_dim_2_2 (a int, b int, r real, primary key (a, b));

CREATE TABLE bloom_snowflake_multi_dim_2 (
	a int, b int,
	id21a int, id21b int,
	id22a int, id22b int,
	r real,
	foreign key (id21a, id21b) references bloom_snowflake_multi_dim_2_1(a, b),
	foreign key (id22a, id22b) references bloom_snowflake_multi_dim_2_2(a, b),
	primary key (a, b));

CREATE TABLE bloom_snowflake_multi_fact (
	id1a int, id1b int,
	id2a int, id2b int,
	padding text,
	foreign key (id1a, id1b) references bloom_snowflake_multi_dim_1(a, b),
	foreign key (id2a, id2b) references bloom_snowflake_multi_dim_2(a, b));

SELECT setseed(0.5);

INSERT INTO bloom_snowflake_multi_dim_1_1 SELECT i, i, random() FROM generate_series(1, 1000) s(i);
INSERT INTO bloom_snowflake_multi_dim_1_2 SELECT i, i, random() FROM generate_series(1, 1000) s(i);

WITH d AS (SELECT i, 1 + mod((100000 * random())::int, 1000) AS a, 1 + mod((100000 * random())::int, 1000) AS b FROM generate_series(1, 1000) s(i))
INSERT INTO bloom_snowflake_multi_dim_1 SELECT i, i, a, a, b, b, random() FROM d;

INSERT INTO bloom_snowflake_multi_dim_2_1 SELECT i, i, random() FROM generate_series(1, 10000) s(i);
INSERT INTO bloom_snowflake_multi_dim_2_2 SELECT i, i, random() FROM generate_series(1, 10000) s(i);

WITH d AS (SELECT i, 1 + mod((100000 * random())::int, 1000) AS a, 1 + mod((100000 * random())::int, 1000) AS b FROM generate_series(1, 1000) s(i))
INSERT INTO bloom_snowflake_multi_dim_2 SELECT i, i, a, a, b, b, random() FROM d;

WITH d AS (SELECT 1 + mod((100000 * random())::int, 1000) AS a, 1 + mod((100000 * random())::int, 1000) AS b, md5(i::text) AS p FROM generate_series(1, 100000) AS s(i))
INSERT INTO bloom_snowflake_multi_fact
SELECT
    a, a, b, b, p
FROM d;

SET default_statistics_target = 1000;

VACUUM ANALYZE;

-- increase the accepted build size (includes the fact)
SET bloom_filter_pushdown_max_build_relids = 4;

EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_snowflake_multi_fact f
JOIN bloom_snowflake_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_snowflake_multi_dim_1_1 d11 ON (d1.id11a = d11.a AND d1.id11b = d11.b)
JOIN bloom_snowflake_multi_dim_1_2 d12 ON (d1.id12a = d12.a AND d1.id12b = d12.b)
WHERE d11.r < 0.45 AND d12.r < 0.55;

EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_snowflake_multi_fact f
JOIN bloom_snowflake_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_snowflake_multi_dim_1_1 d11 ON (d1.id11a = d11.a AND d1.id11b = d11.b)
JOIN bloom_snowflake_multi_dim_1_2 d12 ON (d1.id12a = d12.a AND d1.id12b = d12.b)
WHERE d11.r < 0.75 AND d12.r < 0.75;

EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_snowflake_multi_fact f
JOIN bloom_snowflake_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_snowflake_multi_dim_1_1 d11 ON (d1.id11a = d11.a AND d1.id11b = d11.b)
JOIN bloom_snowflake_multi_dim_1_2 d12 ON (d1.id12a = d12.a AND d1.id12b = d12.b)
JOIN bloom_snowflake_multi_dim_2 d2 ON (f.id2a = d2.a AND f.id2b = d2.b)
JOIN bloom_snowflake_multi_dim_2_1 d21 ON (d2.id21a = d21.a AND d2.id21b = d21.b)
JOIN bloom_snowflake_multi_dim_2_2 d22 ON (d2.id22a = d22.a AND d2.id22b = d22.b)
WHERE d11.r < 0.75 AND d12.r < 0.75;

RESET bloom_filter_pushdown_max_build_relids;

DROP TABLE bloom_snowflake_multi_fact;
DROP TABLE bloom_snowflake_multi_dim_1;
DROP TABLE bloom_snowflake_multi_dim_2;
DROP TABLE bloom_snowflake_multi_dim_1_1;
DROP TABLE bloom_snowflake_multi_dim_1_2;
DROP TABLE bloom_snowflake_multi_dim_2_1;
DROP TABLE bloom_snowflake_multi_dim_2_2;
