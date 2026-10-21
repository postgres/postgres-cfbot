--
-- pgbench-vec (Vector Search & Hybrid RAG Benchmark Workload Suite)
-- Parallel In-Database Data Generation Script
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> \
--           -f workloads/pgbench-vec/pgbench_vec_init.sql
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':items_per_scale' FROM '^[0-9]+$')::int, 200) AS items_per_scale,
    COALESCE(substring(':dim' FROM '^[0-9]+$')::int, 8) AS dim
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

INSERT INTO pgbench_vec_items (id, category, tags, payload, embedding)
SELECT i,
       (i % 10)::int,
       ARRAY[(i % 5)::int, ((i + 2) % 5)::int],
       pgbench.format_string('vec-item-%d-cat-%d', i, i % 10),
       pgbench_vec_parse(pgbench.random_vector(:dim))
  FROM generate_series(:client_id + 1, :items_per_scale * :scale, :num_workers) AS i;
