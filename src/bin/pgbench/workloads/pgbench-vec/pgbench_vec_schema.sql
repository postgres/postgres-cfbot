--
-- pgbench-vec (Vector Search & Hybrid RAG Benchmark Workload Suite)
-- Schema DDL and Helper Functions
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS pgbench_vec_items CASCADE;

CREATE TABLE pgbench_vec_items (
    id bigint NOT NULL,
    category integer NOT NULL DEFAULT 0,
    tags integer[] NOT NULL DEFAULT '{}',
    payload text,
    embedding float4[] NOT NULL
);

--
-- Convert either pgvector literal syntax ('[x1,x2,...]') or PostgreSQL
-- array literal syntax ('{x1,x2,...}') into float4[].
--
CREATE OR REPLACE FUNCTION pgbench_vec_parse(v_text text)
RETURNS float4[]
LANGUAGE sql
IMMUTABLE PARALLEL SAFE STRICT
AS $$
    SELECT translate(v_text, '[]', '{}')::float4[];
$$;

--
-- Compute squared Euclidean (L2) distance between two float4[] vectors.
--
CREATE OR REPLACE FUNCTION pgbench_vec_l2_sq(a float4[], b float4[])
RETURNS float8
LANGUAGE sql
IMMUTABLE PARALLEL SAFE STRICT
AS $$
    SELECT COALESCE(sum((av - bv) * (av - bv)), 0.0)
      FROM unnest(a, b) AS t(av, bv);
$$;
