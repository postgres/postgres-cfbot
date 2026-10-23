--
-- pgbench-vec: Mode 6 Continuous Ingestion, Aging, and Index Degradation
--
-- Executes continuous write mutations interleaved with recall evaluation:
--   - 50% Streaming Ingest: Streaming upserts with causal watermarks & reservoir push
--   - 25% Vector Drift:     Directional embedding drift modifying distance profiles
--   - 25% Catalog Churn:    Soft-deletions simulating item retirements
--   - 100% Search & Recall: Evaluates recall between partitioned view (stale/unvacuumed)
--                           and full active catalog to measure index degradation.
--

\set dim 8
\set k 5

-- 1. Continuous Write Mutation
\set op_roll random(1, 100)

\if :op_roll <= 50
    -- Streaming Vector Ingestion & Causal Watermark Advancement
    \set new_id random(2000000, 9999999)
    \set cat random(0, 9)
    \set tag1 random(0, 4)
    \set tag2 random(0, 4)
    \set v random_unit_vector(:dim)

    INSERT INTO pgbench_vec_items (id, category, price, is_active, tags, payload, metadata, embedding, image_embedding)
    VALUES (:new_id, :cat, 49.99, true, ARRAY[:tag1, :tag2], 'streaming-aging-item',
            '{"streamed": true, "churn": "mode6"}'::jsonb,
            pgbench_vec_parse(:v), pgbench_vec_parse(:v))
    ON CONFLICT (id) DO UPDATE
       SET embedding = EXCLUDED.embedding,
           category = EXCLUDED.category,
           is_active = true
    RETURNING id \gset ins_

    \if reservoir_count('vec_ids') >= 0
    \reservoir_push vec_ids :ins_id
    \endif
    \advance_watermark :ins_id

\elif :op_roll <= 75
    -- Vector Drift Update
    \set drift_id random(1, 200 * :scale)
    \set drifted_vec planted_vector(:dim, :drift_id, 0.25, 42)

    UPDATE pgbench_vec_items
       SET embedding = pgbench_vec_parse(:drifted_vec)
     WHERE id = :drift_id;

\else
    -- Catalog Churn & Soft Deletion
    \set churn_id random(1, 200 * :scale)

    UPDATE pgbench_vec_items
       SET is_active = false
     WHERE id = :churn_id;

\endif

-- 2. Approximate Top-K Search with Aging Recall Telemetry
\set qcat random(0, 9)
\set qvec clustered_vector(:dim, 10, 0.25, 42, :qcat)

SELECT id
  FROM pgbench_vec_items
 WHERE is_active = true
   AND id <= 200 * :scale
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col retrieved

SELECT id
  FROM pgbench_vec_items
 WHERE is_active = true
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col ground_truth

\record_recall name=r_aging retrieved ground_truth :k
