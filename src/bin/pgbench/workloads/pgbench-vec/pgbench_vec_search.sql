--
-- pgbench-vec: Top-K Vector Similarity Search with Recall@K Telemetry
--
-- Generates a unit-normalized query vector via random_vector(), retrieves
-- top-K nearest neighbors using \gset_col, and records Recall@K via
-- \record_recall.
--

\set dim 8
\set k 10
\set qvec random_vector(:dim)

-- Approximate candidate scan (e.g., category-pruned or index probe)
SELECT id
  FROM pgbench_vec_items
 WHERE id <= 200 * :scale
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col retrieved

-- Ground-truth exact top-K scan
SELECT id
  FROM pgbench_vec_items
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col ground_truth

\record_recall retrieved ground_truth :k
