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
    COALESCE(substring(':dim' FROM '^[0-9]+$')::int, 8) AS dim,
    COALESCE(substring(':cluster_seed' FROM '^-?[0-9]+$')::bigint, 42) AS cluster_seed
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

INSERT INTO pgbench_vec_items (id, category, price, is_active, tags, payload, metadata, embedding, image_embedding)
SELECT i,
       (i % 10)::int,
       (10.0 + ((i * 17) % 900) * 0.1)::numeric(10,2),
       ((i % 7) != 0),
       ARRAY[(i % 5)::int, ((i + 2) % 5)::int],
       pgbench.format_string('vec-item-%d-cat-%d', i, i % 10),
       jsonb_build_object(
           'item_id', i,
           'brand', pgbench.format_string('brand-%d', (i % 8)::int),
           'rating', round((3.0 + ((i % 21) * 0.1))::numeric, 1),
           'in_stock', ((i % 5) != 0)
       ),
       pgbench_vec_parse(pgbench.clustered_vector(:dim, 10, 0.15, :cluster_seed, (i % 10)::int)),
       pgbench_vec_parse(pgbench.random_unit_vector(:dim))
  FROM generate_series(:client_id + 1, :items_per_scale * :scale, :num_workers) AS i;
