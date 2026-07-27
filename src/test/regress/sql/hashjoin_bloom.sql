-- tests to validate bloom filter pushdown
-- no parallel query support for now
SET max_parallel_workers_per_gather = 0;

-- a couple very simple join queries
CREATE TABLE bloom_simple_dim (id int, r real);
CREATE TABLE bloom_simple_fact (id int, padding text);

INSERT INTO bloom_simple_dim SELECT i, random() FROM generate_series(1,1000) s(i);

INSERT INTO bloom_simple_fact
SELECT
    1 + mod(i, 1000),
    md5(i::text)
FROM generate_series(1,100000) s(i);

VACUUM ANALYZE;

-- simple query, no join is selective enough for a filter
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_simple_fact f
JOIN bloom_simple_dim d ON (f.id = d.id);

-- join is 75% selective (not enough for a filter to be created)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_simple_fact f
JOIN bloom_simple_dim d ON (f.id = d.id)
WHERE d.r < 0.75;

-- join is 50% selective (enough for a filter to be pushed down)
EXPLAIN (COSTS OFF)
SELECT *
FROM bloom_simple_fact f
JOIN bloom_simple_dim d ON (f.id = d.id)
WHERE d.r < 0.5;

DROP TABLE bloom_simple_dim;
DROP TABLE bloom_simple_fact;

-- a schema with multi-column foreign key, to test estimates
CREATE TABLE bloom_multi_dim (id1 int, id2 int, r real, primary key (id1, id2));
CREATE TABLE bloom_multi_fact (id1 int, id2 int, padding text, foreign key (id1, id2) references bloom_multi_dim(id1,id2));

INSERT INTO bloom_multi_dim SELECT i, i, (i / 1000.0) FROM generate_series(1,1000) s(i);

INSERT INTO bloom_multi_fact
SELECT
    1 + mod(i, 1000),
    1 + mod(i, 1000),
    md5(i::text)
FROM generate_series(1,30000) s(i);

VACUUM ANALYZE;

-- simple query, no join is selective enough for a filter
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_multi_fact f
JOIN bloom_multi_dim d ON (f.id1 = d.id1 AND f.id2 = d.id2);

-- join is 75% selective (not enough for a filter to be created)
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_multi_fact f
JOIN bloom_multi_dim d ON (f.id1 = d.id1 AND f.id2 = d.id2)
WHERE d.r < 0.75;

-- join is 50% selective (enough for a filter to be pushed down)
EXPLAIN (ANALYZE, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
SELECT *
FROM bloom_multi_fact f
JOIN bloom_multi_dim d ON (f.id1 = d.id1 AND f.id2 = d.id2)
WHERE d.r < 0.5;

DROP TABLE bloom_multi_fact;
DROP TABLE bloom_multi_dim;
