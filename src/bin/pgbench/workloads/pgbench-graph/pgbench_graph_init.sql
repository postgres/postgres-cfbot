--
-- pgbench-graph (LDBC-Inspired Social Network Graph Benchmark Workload Suite)
-- Parallel In-Database Data Generation Script
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> \
--           -f workloads/pgbench-graph/pgbench_graph_init.sql
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':persons_per_scale' FROM '^[0-9]+$')::int, 200) AS persons_per_scale
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Persons (partitioned across workers)
INSERT INTO pgbench_graph_persons (id, first_name, last_name, city_id, created_ts)
SELECT p,
       pgbench.format_string('First%d', p),
       pgbench.c_last((p % 1000)::int),
       1 + (p % 50)::int,
       1700000000000000::bigint + p * 1000
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p;

-- 2. Bidirectional knows edges (~4 edges per person with Zipfian degree skew)
INSERT INTO pgbench_graph_knows (src_id, dst_id, weight, created_ts)
SELECT DISTINCT
       edges.a,
       edges.b,
       round((0.1 + (edges.a % 10) * 0.1)::numeric, 2)::float8,
       1700000000000000::bigint + edges.a * 1000 + edges.b
  FROM (
      SELECT p AS src,
             1 + ((p + k * 7 + pgbench.random_zipfian(1, :persons_per_scale * :scale, 1.2) - 1)
                  % (:persons_per_scale * :scale)) AS dst
        FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
        CROSS JOIN generate_series(1, 2) AS k
  ) AS raw_pairs
  CROSS JOIN LATERAL (
      VALUES (raw_pairs.src, raw_pairs.dst),
             (raw_pairs.dst, raw_pairs.src)
  ) AS edges(a, b)
 WHERE raw_pairs.src <> raw_pairs.dst;

-- 3. Messages (~2 messages per person)
INSERT INTO pgbench_graph_messages (id, creator_id, content, created_ts)
SELECT (p - 1) * 2 + m,
       p,
       pgbench.format_string('Post #%d by person %d: %s', m, p, pgbench.random_astring(16, 32)),
       1700000000000000::bigint + p * 2000 + m * 500
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 2) AS m;
