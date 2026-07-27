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

DROP TABLE bloom_star_dim_1;
DROP TABLE bloom_star_dim_2;
DROP TABLE bloom_star_dim_3;
DROP TABLE bloom_star_dim_4;
DROP TABLE bloom_star_dim_5;
DROP TABLE bloom_star_dim_6;
DROP TABLE bloom_star_dim_7;
DROP TABLE bloom_star_fact;


-- schema with multi-column FK joins

CREATE TABLE bloom_star_multi_dim_1 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_2 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_3 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_4 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_5 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_6 (a int, b int, r real, primary key (a, b));
CREATE TABLE bloom_star_multi_dim_7 (a int, b int, r real, primary key (a, b));

CREATE TABLE bloom_star_multi_fact (
	id1a int, id1b int,
	id2a int, id2b int,
	id3a int, id3b int,
	id4a int, id4b int,
	id5a int, id5b int,
	id6a int, id6b int,
	id7a int, id7b int,
	FOREIGN KEY (id1a, id1b) REFERENCES bloom_star_multi_dim_1 (a, b),
	FOREIGN KEY (id2a, id2b) REFERENCES bloom_star_multi_dim_2 (a, b),
	FOREIGN KEY (id3a, id3b) REFERENCES bloom_star_multi_dim_3 (a, b),
	FOREIGN KEY (id4a, id4b) REFERENCES bloom_star_multi_dim_4 (a, b),
	FOREIGN KEY (id5a, id5b) REFERENCES bloom_star_multi_dim_5 (a, b),
	FOREIGN KEY (id6a, id6b) REFERENCES bloom_star_multi_dim_6 (a, b),
	FOREIGN KEY (id7a, id7b) REFERENCES bloom_star_multi_dim_7 (a, b),
	padding text);

SELECT setseed(1);

INSERT INTO bloom_star_multi_dim_1 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_2 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_3 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_4 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_5 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_6 SELECT i, i, random() FROM generate_series(1,1000) s(i);
INSERT INTO bloom_star_multi_dim_7 SELECT i, i, random() FROM generate_series(1,1000) s(i);

WITH d AS (
    SELECT
        1 + mod((100000 * random())::int, 1000) AS id1,
        1 + mod((100000 * random())::int, 1000) AS id2,
        1 + mod((100000 * random())::int, 1000) AS id3,
        1 + mod((100000 * random())::int, 1000) AS id4,
        1 + mod((100000 * random())::int, 1000) AS id5,
        1 + mod((100000 * random())::int, 1000) AS id6,
        1 + mod((100000 * random())::int, 1000) AS id7,
        md5(i::text) AS p
    FROM generate_series(1,100000) s(i)
)
INSERT INTO bloom_star_multi_fact
SELECT
    id1, id1,
    id2, id2,
    id3, id3,
    id4, id4,
    id5, id5,
    id6, id6,
    id7, id7,
    p
FROM d;

SET default_statistics_target = 1000;

VACUUM ANALYZE;

-- first join is 50% selective (good enough for a filter push down)
-- the join will be executed done last
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_star_multi_fact f
JOIN bloom_star_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_star_multi_dim_2 d2 ON (f.id2a = d2.a AND f.id2b = d2.b)
JOIN bloom_star_multi_dim_3 d3 ON (f.id3a = d3.a AND f.id3b = d3.b)
JOIN bloom_star_multi_dim_4 d4 ON (f.id4a = d4.a AND f.id4b = d4.b)
JOIN bloom_star_multi_dim_5 d5 ON (f.id5a = d5.a AND f.id5b = d5.b)
JOIN bloom_star_multi_dim_6 d6 ON (f.id6a = d6.a AND f.id6b = d6.b)
JOIN bloom_star_multi_dim_7 d7 ON (f.id7a = d7.a AND f.id7b = d7.b)
WHERE d1.r < 0.5;

-- two joins 50% selective
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_star_multi_fact f
JOIN bloom_star_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_star_multi_dim_2 d2 ON (f.id2a = d2.a AND f.id2b = d2.b)
JOIN bloom_star_multi_dim_3 d3 ON (f.id3a = d3.a AND f.id3b = d3.b)
JOIN bloom_star_multi_dim_4 d4 ON (f.id4a = d4.a AND f.id4b = d4.b)
JOIN bloom_star_multi_dim_5 d5 ON (f.id5a = d5.a AND f.id5b = d5.b)
JOIN bloom_star_multi_dim_6 d6 ON (f.id6a = d6.a AND f.id6b = d6.b)
JOIN bloom_star_multi_dim_7 d7 ON (f.id7a = d7.a AND f.id7b = d7.b)
WHERE d1.r < 0.4 AND d7.r < 0.5;

-- all joins selective for a filter (more than how many we allow)
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_star_multi_fact f
JOIN bloom_star_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_star_multi_dim_2 d2 ON (f.id2a = d2.a AND f.id2b = d2.b)
JOIN bloom_star_multi_dim_3 d3 ON (f.id3a = d3.a AND f.id3b = d3.b)
JOIN bloom_star_multi_dim_4 d4 ON (f.id4a = d4.a AND f.id4b = d4.b)
JOIN bloom_star_multi_dim_5 d5 ON (f.id5a = d5.a AND f.id5b = d5.b)
JOIN bloom_star_multi_dim_6 d6 ON (f.id6a = d6.a AND f.id6b = d6.b)
JOIN bloom_star_multi_dim_7 d7 ON (f.id7a = d7.a AND f.id7b = d7.b)
WHERE d1.r < 0.3 AND d2.r < 0.35 AND d3.r < 0.4 AND d4.r < 0.45 AND d5.r < 0.5 AND d6.r < 0.55 AND d7.r < 0.6;


SET bloom_filter_pushdown_max = 10;

-- all joins selective for a filter
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_star_multi_fact f
JOIN bloom_star_multi_dim_1 d1 ON (f.id1a = d1.a AND f.id1b = d1.b)
JOIN bloom_star_multi_dim_2 d2 ON (f.id2a = d2.a AND f.id2b = d2.b)
JOIN bloom_star_multi_dim_3 d3 ON (f.id3a = d3.a AND f.id3b = d3.b)
JOIN bloom_star_multi_dim_4 d4 ON (f.id4a = d4.a AND f.id4b = d4.b)
JOIN bloom_star_multi_dim_5 d5 ON (f.id5a = d5.a AND f.id5b = d5.b)
JOIN bloom_star_multi_dim_6 d6 ON (f.id6a = d6.a AND f.id6b = d6.b)
JOIN bloom_star_multi_dim_7 d7 ON (f.id7a = d7.a AND f.id7b = d7.b)
WHERE d1.r < 0.3 AND d2.r < 0.35 AND d3.r < 0.4 AND d4.r < 0.45 AND d5.r < 0.5 AND d6.r < 0.55 AND d7.r < 0.6;

RESET bloom_filter_pushdown_max;
RESET join_collapse_limit;

DROP TABLE bloom_star_multi_fact;
DROP TABLE bloom_star_multi_dim_1;
DROP TABLE bloom_star_multi_dim_2;
DROP TABLE bloom_star_multi_dim_3;
DROP TABLE bloom_star_multi_dim_4;
DROP TABLE bloom_star_multi_dim_5;
DROP TABLE bloom_star_multi_dim_6;
DROP TABLE bloom_star_multi_dim_7;
