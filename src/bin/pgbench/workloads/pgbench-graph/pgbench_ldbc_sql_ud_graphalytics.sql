--
-- pgbench-graph (SQL_UD): LDBC Graphalytics Suite (BFS, WCC, SSSP, PageRank, LCC)
-- Uses WITH RECURSIVE ... UNION DISTINCT ON (...) supported by postsql/postgres
-- one-with-everything for BFS, WCC, and SSSP.
--

\set root_id 1

-- 1. Breadth-First Search (BFS) via WITH RECURSIVE ... UNION DISTINCT ON (vertex_id)
--    Eliminates ARRAY path tracking and post-recursion GROUP BY deduplication.
WITH RECURSIVE bfs AS (
    SELECT p.id AS vertex_id,
           0 AS min_depth,
           0::bigint AS parent_id
      FROM pgbench_graph_persons p
     WHERE p.id = :root_id
    UNION DISTINCT ON (vertex_id)
    SELECT k.dst_id AS vertex_id,
           b.min_depth + 1 AS min_depth,
           b.vertex_id AS parent_id
      FROM bfs b
      JOIN pgbench_graph_knows k ON k.src_id = b.vertex_id
     WHERE b.min_depth < 4
)
SELECT count(*) AS bfs_reachable,
       COALESCE(MIN(min_depth) FILTER (WHERE vertex_id = :root_id), -1) AS bfs_root_depth,
       COALESCE(MAX(min_depth), 0) AS bfs_max_depth
  FROM bfs \gset

\validate_result :bfs_root_depth 0
\validate_result (:bfs_reachable >= 1 and :bfs_max_depth >= 0) true

-- 2. Weakly Connected Components (WCC) via WITH RECURSIVE ... UNION DISTINCT ON (vid ORDER BY cid ASC)
WITH RECURSIVE wcc AS (
    SELECT id AS vid,
           id AS cid,
           0 AS round
      FROM pgbench_graph_persons
    UNION DISTINCT ON (vid ORDER BY cid ASC)
    SELECT k.dst_id AS vid,
           w.cid AS cid,
           w.round + 1 AS round
      FROM wcc w
      JOIN pgbench_graph_knows k ON k.src_id = w.vid
     WHERE w.round < 4
)
SELECT count(DISTINCT cid) AS wcc_components,
       count(*) AS wcc_vertices
  FROM wcc \gset

\validate_result (:wcc_components >= 1 and :wcc_components <= :wcc_vertices) true

-- 3. Single-Source Shortest Paths (SSSP) over weighted pgbench_graph_knows via UNION DISTINCT ON (vid ORDER BY min_dist ASC)
WITH RECURSIVE sssp AS (
    SELECT :root_id::bigint AS vid,
           0.0::float8 AS min_dist,
           0 AS hops
    UNION DISTINCT ON (vid ORDER BY min_dist ASC)
    SELECT k.dst_id AS vid,
           s.min_dist + k.weight AS min_dist,
           s.hops + 1 AS hops
      FROM sssp s
      JOIN pgbench_graph_knows k ON k.src_id = s.vid
     WHERE s.hops < 3
       AND k.dst_id <> :root_id
)
SELECT COALESCE(MIN(min_dist) FILTER (WHERE vid = :root_id), -1.0) AS sssp_root_dist,
       COALESCE(MAX(min_dist), 0.0) AS sssp_max_dist
  FROM sssp \gset

\validate_result :sssp_root_dist 0.0 0.000001
\validate_result (:sssp_max_dist >= 0.0) true

-- 4. PageRank (PR) — 3-step damped power iteration (d = 0.85) with mass conservation check
WITH n_cte AS (
    SELECT GREATEST(count(*), 1)::float8 AS n FROM pgbench_graph_persons
),
deg AS (
    SELECT p.id AS vid,
           count(k.dst_id)::int AS out_deg
      FROM pgbench_graph_persons p
      LEFT JOIN pgbench_graph_knows k ON k.src_id = p.id
     GROUP BY p.id
),
pr0 AS (
    SELECT d.vid, d.out_deg, (1.0 / n_cte.n) AS pr
      FROM deg d CROSS JOIN n_cte
),
dangling0 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr0 WHERE out_deg = 0
),
pr1 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d0.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr0 p
      CROSS JOIN n_cte
      CROSS JOIN dangling0 d0
      LEFT JOIN pgbench_graph_knows k ON k.dst_id = p.vid
      LEFT JOIN pr0 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d0.dmass
),
dangling1 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr1 WHERE out_deg = 0
),
pr2 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d1.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr1 p
      CROSS JOIN n_cte
      CROSS JOIN dangling1 d1
      LEFT JOIN pgbench_graph_knows k ON k.dst_id = p.vid
      LEFT JOIN pr1 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d1.dmass
)
SELECT round(sum(pr)::numeric, 6)::float8 AS pr_total_mass,
       COALESCE(max(pr), 0.0) >= COALESCE(min(pr), 0.0) AS pr_ordered
  FROM pr2 \gset

\validate_result :pr_total_mass 1.0 0.01
\validate_result :pr_ordered true

-- 5. Local Clustering Coefficient (LCC) per vertex: 2 * triangles(v) / (deg(v) * (deg(v) - 1))
WITH deg AS (
    SELECT src_id AS vid, count(DISTINCT dst_id) AS d
      FROM pgbench_graph_knows
     GROUP BY src_id
),
tri AS (
    SELECT k1.src_id AS vid,
           count(*) AS closed_pairs
      FROM pgbench_graph_knows k1
      JOIN pgbench_graph_knows k2 ON k2.src_id = k1.src_id AND k2.dst_id > k1.dst_id
      JOIN pgbench_graph_knows k3 ON k3.src_id = k1.dst_id AND k3.dst_id = k2.dst_id
     GROUP BY k1.src_id
),
lcc_per_vertex AS (
    SELECT deg.vid,
           CASE
               WHEN deg.d < 2 THEN 0.0::float8
               ELSE LEAST(1.0::float8, (2.0 * COALESCE(tri.closed_pairs, 0))::float8 / (deg.d * (deg.d - 1))::float8)
           END AS lcc
      FROM deg
      LEFT JOIN tri ON tri.vid = deg.vid
)
SELECT COALESCE(avg(lcc), 0.0)::float8 AS avg_lcc
  FROM lcc_per_vertex \gset

\validate_result (:avg_lcc >= 0.0 and :avg_lcc <= 1.0) true
