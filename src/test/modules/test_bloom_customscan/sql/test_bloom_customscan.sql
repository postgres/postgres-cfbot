CREATE EXTENSION test_bloom_customscan;
-- ensure the library (and its planner hook + GUC) is loaded
LOAD 'test_bloom_customscan';

-- Force a serial hash join, which is where the bloom-filter pushdown applies.
SET enable_hashjoin_bloom = on;
SET enable_mergejoin = off;
SET enable_nestloop = off;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE cs_fact (a int, b int, payload int);
CREATE TABLE cs_dim (id int, id2 int, label text);

-- Fact keys range over 0..999; the dimension only covers 1..50, so the join
-- is highly selective and the pushed-down filter should reject most fact rows.
INSERT INTO cs_fact
  SELECT g % 1000, g % 1000, g FROM generate_series(1, 20000) g;
INSERT INTO cs_dim
  SELECT g, g, 'x' || g FROM generate_series(1, 50) g;

ANALYZE cs_fact;
ANALYZE cs_dim;

-- Offer the bloom-capable custom scan and force it to be chosen (the CustomPath
-- keeps disabled_nodes = 0, so disabling seqscan makes it win).
SET test_bloom_customscan.enable = on;
SET enable_seqscan = off;

-- The fact relation is scanned by our Custom Scan and receives the filter.
EXPLAIN (COSTS OFF, VERBOSE)
SELECT count(*) FROM cs_fact f JOIN cs_dim d ON f.a = d.id;

-- Single-key join: the filter must actually reject fact rows.
SELECT test_bloom_cs_reset();
SELECT count(*) FROM cs_fact f JOIN cs_dim d ON f.a = d.id;
SELECT test_bloom_cs_rejected_rows() > 0 AS filter_rejected_rows;

-- Correctness: the result must be identical with and without the filter.
SET test_bloom_customscan.enable = on;
SET enable_seqscan = off;
CREATE TEMP TABLE r_cs AS
  SELECT f.a, count(*) AS n FROM cs_fact f JOIN cs_dim d ON f.a = d.id GROUP BY f.a;

SET test_bloom_customscan.enable = off;
SET enable_seqscan = on;
CREATE TEMP TABLE r_plain AS
  SELECT f.a, count(*) AS n FROM cs_fact f JOIN cs_dim d ON f.a = d.id GROUP BY f.a;

SELECT count(*) AS cs_minus_plain
  FROM (SELECT * FROM r_cs EXCEPT SELECT * FROM r_plain) x;
SELECT count(*) AS plain_minus_cs
  FROM (SELECT * FROM r_plain EXCEPT SELECT * FROM r_cs) x;

-- Two-key join: the opted-in recipient also gets per-key filters built.
SET test_bloom_customscan.enable = on;
SET enable_seqscan = off;
SELECT test_bloom_cs_reset();
SELECT count(*) FROM cs_fact f JOIN cs_dim d ON f.a = d.id AND f.b = d.id2;
SELECT test_bloom_cs_perkey_built() AS perkey_filters_built;
SELECT test_bloom_cs_rejected_rows() > 0 AS filter_rejected_rows;

-- cleanup
SET test_bloom_customscan.enable = off;
SET enable_seqscan = on;
DROP TABLE cs_fact, cs_dim;
DROP EXTENSION test_bloom_customscan;
