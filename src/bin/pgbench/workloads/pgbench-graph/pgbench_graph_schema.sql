--
-- pgbench-graph (LDBC-Inspired Social Network Graph Benchmark Workload Suite)
-- Schema DDL and Stored Procedures
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS pgbench_graph_messages CASCADE;
DROP TABLE IF EXISTS pgbench_graph_knows CASCADE;
DROP TABLE IF EXISTS pgbench_graph_persons CASCADE;
DROP SEQUENCE IF EXISTS pgbench_graph_msg_seq CASCADE;

CREATE TABLE pgbench_graph_persons (
    id bigint NOT NULL,
    first_name text NOT NULL,
    last_name text NOT NULL,
    city_id integer NOT NULL,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_knows (
    src_id bigint NOT NULL,
    dst_id bigint NOT NULL,
    weight float8 NOT NULL DEFAULT 1.0,
    created_ts bigint NOT NULL
);

CREATE SEQUENCE pgbench_graph_msg_seq START WITH 100000;

CREATE TABLE pgbench_graph_messages (
    id bigint NOT NULL DEFAULT nextval('pgbench_graph_msg_seq'),
    creator_id bigint NOT NULL,
    content text NOT NULL,
    created_ts bigint NOT NULL
);

--
-- Transactional graph mutation procedure: inserts a bidirectional edge in
-- pgbench_graph_knows and a new activity message in pgbench_graph_messages.
--
CREATE OR REPLACE PROCEDURE pgbench_graph_add_edge(
    p_src bigint,
    p_dst bigint,
    p_weight float8,
    p_content text,
    INOUT p_msg_id bigint DEFAULT 0
)
LANGUAGE plpgsql
AS $$
DECLARE
    v_ts bigint := (extract(epoch FROM clock_timestamp()) * 1000000)::bigint;
BEGIN
    IF p_src <> p_dst THEN
        INSERT INTO pgbench_graph_knows (src_id, dst_id, weight, created_ts)
        VALUES (p_src, p_dst, p_weight, v_ts),
               (p_dst, p_src, p_weight, v_ts)
        ON CONFLICT (src_id, dst_id) DO UPDATE
           SET weight = EXCLUDED.weight,
               created_ts = EXCLUDED.created_ts;
    END IF;

    INSERT INTO pgbench_graph_messages (creator_id, content, created_ts)
    VALUES (p_src, p_content, v_ts)
    RETURNING id INTO p_msg_id;
END;
$$;
