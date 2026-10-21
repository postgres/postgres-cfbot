--
-- pgbench-vec (Vector Search & Hybrid RAG Benchmark Workload Suite)
-- Constraints, Indexes, and Statistics
--

SET client_min_messages = warning;

ALTER TABLE pgbench_vec_items
    ADD CONSTRAINT pgbench_vec_items_pkey PRIMARY KEY (id);

CREATE INDEX pgbench_vec_items_category_idx
    ON pgbench_vec_items (category);

CREATE INDEX pgbench_vec_items_tags_gin_idx
    ON pgbench_vec_items USING gin (tags);

ANALYZE pgbench_vec_items;
