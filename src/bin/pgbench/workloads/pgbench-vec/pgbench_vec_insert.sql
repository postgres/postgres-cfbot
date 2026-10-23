--
-- pgbench-vec: Streaming Vector Upsert & Causal Watermark Advancement
--

\set dim 8
\set new_id random(1000000, 9999999)
\set cat random(0, 9)
\set tag1 random(0, 4)
\set tag2 random(0, 4)
\set v random_vector(:dim)

INSERT INTO pgbench_vec_items (id, category, tags, payload, embedding)
VALUES (:new_id, :cat, ARRAY[:tag1, :tag2], 'streamed-item', pgbench_vec_parse(:v))
ON CONFLICT (id) DO UPDATE
   SET embedding = EXCLUDED.embedding,
       category = EXCLUDED.category
RETURNING id \gset ins_

\if reservoir_count('vec_ids') >= 0
\reservoir_push vec_ids :ins_id
\endif
\advance_watermark :ins_id
