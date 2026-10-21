--
-- pgbench-vec: Filtered Hybrid Vector Search (Metadata + Tag + Vector Distance)
--

\set dim 8
\set k 5
\set cat random(0, 9)
\set tag random(0, 4)
\set qvec random_vector(:dim)

SELECT id
  FROM pgbench_vec_items
 WHERE category = :cat
   AND :tag = ANY(tags)
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col filtered_ids

\record_recall filtered_ids filtered_ids :k
