--
-- pgbench-vec: Ground Truth Precomputation Script
--
-- Populates pgbench_vec_queries and computes exact ground truths into
-- pgbench_vec_ground_truth and pgbench_vec_queries.truth_ids.
--

SET client_min_messages = warning;

INSERT INTO pgbench_vec_queries (query_id, filter_category, min_price, max_price, text_query, image_query)
SELECT q,
       (q % 10)::int,
       10.00::numeric,
       80.00::numeric,
       pgbench_vec_parse(pgbench.random_unit_vector(8)),
       pgbench_vec_parse(pgbench.random_unit_vector(8))
  FROM generate_series(1, 20) AS q
ON CONFLICT (query_id) DO NOTHING;

-- Populate unfiltered ground truth
INSERT INTO pgbench_vec_ground_truth (query_id, filter_mode, res_id, rank, distance)
SELECT q.query_id,
       'unfiltered',
       top.id,
       top.rn::int,
       top.dist
  FROM pgbench_vec_queries q
 CROSS JOIN LATERAL (
    SELECT i.id,
           row_number() OVER (ORDER BY pgbench_vec_l2_sq(i.embedding, q.text_query) ASC, i.id ASC) AS rn,
           pgbench_vec_l2_sq(i.embedding, q.text_query) AS dist
      FROM pgbench_vec_items i
     ORDER BY pgbench_vec_l2_sq(i.embedding, q.text_query) ASC, i.id ASC
     LIMIT 10
 ) top
ON CONFLICT (query_id, filter_mode, rank) DO UPDATE
   SET res_id = EXCLUDED.res_id,
       distance = EXCLUDED.distance;

-- Update truth_ids array in pgbench_vec_queries
UPDATE pgbench_vec_queries q
   SET truth_ids = ARRAY(
       SELECT res_id
         FROM pgbench_vec_ground_truth gt
        WHERE gt.query_id = q.query_id
          AND gt.filter_mode = 'unfiltered'
        ORDER BY gt.rank ASC
   );

ANALYZE pgbench_vec_queries;
ANALYZE pgbench_vec_ground_truth;
