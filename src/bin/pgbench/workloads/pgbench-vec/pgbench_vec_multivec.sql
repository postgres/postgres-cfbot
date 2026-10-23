--
-- pgbench-vec: Mode 5 Multi-Vector / Late-Interaction Hybrid Search
--
-- Combines primary text embedding with auxiliary visual embedding
-- using linear score fusion: dist = alpha * dist_text + (1 - alpha) * dist_image.
--

\set dim 8
\set k 5
\set alpha 0.6
\set qtext random_unit_vector(:dim)
\set qimage random_unit_vector(:dim)

-- Approximate / Hybrid multi-vector search
WITH scored AS (
    SELECT id,
           (:alpha * pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qtext)) +
            (1.0 - :alpha) * pgbench_vec_l2_sq(image_embedding, pgbench_vec_parse(:qimage))) AS combined_dist
      FROM pgbench_vec_items
     WHERE id <= 200 * :scale
     ORDER BY combined_dist ASC, id ASC
     LIMIT :k
)
SELECT id FROM scored
\gset_col retrieved

-- Exact ground-truth for multi-vector score fusion
WITH truth AS (
    SELECT id,
           (:alpha * pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qtext)) +
            (1.0 - :alpha) * pgbench_vec_l2_sq(image_embedding, pgbench_vec_parse(:qimage))) AS combined_dist
      FROM pgbench_vec_items
     ORDER BY combined_dist ASC, id ASC
     LIMIT :k
)
SELECT id FROM truth
\gset_col ground_truth

\record_recall name=r_multivec retrieved ground_truth :k
