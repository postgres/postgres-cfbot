--
-- pgbench-vec (Vector Search & Hybrid RAG Benchmark Workload Suite)
-- Schema DDL and Helper Functions
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS pgbench_vec_ground_truth CASCADE;
DROP TABLE IF EXISTS pgbench_vec_queries CASCADE;
DROP TABLE IF EXISTS pgbench_vec_items CASCADE;

CREATE TABLE pgbench_vec_items (
    id bigint NOT NULL,
    category integer NOT NULL DEFAULT 0,
    price numeric(10,2) NOT NULL DEFAULT 29.99,
    is_active boolean NOT NULL DEFAULT true,
    tags integer[] NOT NULL DEFAULT '{}',
    payload text,
    metadata jsonb NOT NULL DEFAULT '{}'::jsonb,
    embedding float4[] NOT NULL,
    image_embedding float4[] NOT NULL DEFAULT '{}'::float4[]
);

CREATE TABLE pgbench_vec_queries (
    query_id integer PRIMARY KEY,
    filter_category integer,
    min_price numeric(10,2),
    max_price numeric(10,2),
    text_query float4[] NOT NULL,
    image_query float4[] NOT NULL,
    truth_ids bigint[]
);

CREATE TABLE pgbench_vec_ground_truth (
    query_id integer NOT NULL,
    filter_mode text NOT NULL DEFAULT 'unfiltered',
    res_id bigint NOT NULL,
    rank integer NOT NULL,
    distance float8,
    PRIMARY KEY (query_id, filter_mode, rank)
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

--
-- Compute dot product between two float4[] vectors.
--
CREATE OR REPLACE FUNCTION pgbench_vec_dot(a float4[], b float4[])
RETURNS float8
LANGUAGE sql
IMMUTABLE PARALLEL SAFE STRICT
AS $$
    SELECT COALESCE(sum(av * bv), 0.0)
      FROM unnest(a, b) AS t(av, bv);
$$;

--
-- Compute cosine distance (1.0 - cosine_similarity) between two float4[] vectors.
--
CREATE OR REPLACE FUNCTION pgbench_vec_cosine(a float4[], b float4[])
RETURNS float8
LANGUAGE sql
IMMUTABLE PARALLEL SAFE STRICT
AS $$
    SELECT 1.0 - COALESCE(sum(av * bv) / NULLIF(sqrt(sum(av * av)) * sqrt(sum(bv * bv)), 0.0), 1.0)
      FROM unnest(a, b) AS t(av, bv);
$$;
