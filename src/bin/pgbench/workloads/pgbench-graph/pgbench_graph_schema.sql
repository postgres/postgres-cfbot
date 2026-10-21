--
-- pgbench-graph (Unified LDBC Social Network & FinBench Graph Workload Suite)
-- Schema DDL and Stored Procedures
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS pgbench_fin_sign_ins CASCADE;
DROP TABLE IF EXISTS pgbench_fin_mediums CASCADE;
DROP TABLE IF EXISTS pgbench_fin_loans CASCADE;
DROP TABLE IF EXISTS pgbench_fin_transfers CASCADE;
DROP TABLE IF EXISTS pgbench_fin_accounts CASCADE;

DROP TABLE IF EXISTS pgbench_graph_message_tags CASCADE;
DROP TABLE IF EXISTS pgbench_graph_likes CASCADE;
DROP TABLE IF EXISTS pgbench_graph_messages CASCADE;
DROP TABLE IF EXISTS pgbench_graph_forum_members CASCADE;
DROP TABLE IF EXISTS pgbench_graph_forums CASCADE;
DROP TABLE IF EXISTS pgbench_graph_person_tags CASCADE;
DROP TABLE IF EXISTS pgbench_graph_knows CASCADE;
DROP TABLE IF EXISTS pgbench_graph_persons CASCADE;
DROP TABLE IF EXISTS pgbench_graph_tags CASCADE;
DROP TABLE IF EXISTS pgbench_graph_places CASCADE;

DROP SEQUENCE IF EXISTS pgbench_graph_person_seq CASCADE;
DROP SEQUENCE IF EXISTS pgbench_graph_forum_seq CASCADE;
DROP SEQUENCE IF EXISTS pgbench_graph_msg_seq CASCADE;
DROP SEQUENCE IF EXISTS pgbench_fin_transfer_seq CASCADE;
DROP SEQUENCE IF EXISTS pgbench_fin_loan_seq CASCADE;

CREATE SEQUENCE pgbench_graph_person_seq START WITH 100000;
CREATE SEQUENCE pgbench_graph_forum_seq START WITH 100000;
CREATE SEQUENCE pgbench_graph_msg_seq START WITH 100000;
CREATE SEQUENCE pgbench_fin_transfer_seq START WITH 100000;
CREATE SEQUENCE pgbench_fin_loan_seq START WITH 100000;

-- 1. LDBC SNB Static / Dictionary Entities
CREATE TABLE pgbench_graph_places (
    id bigint NOT NULL,
    name text NOT NULL,
    type text NOT NULL,
    part_of_id bigint
);

CREATE TABLE pgbench_graph_tags (
    id bigint NOT NULL,
    name text NOT NULL,
    tagclass_id integer NOT NULL
);

-- 2. LDBC SNB Dynamic Entities & Edges
CREATE TABLE pgbench_graph_persons (
    id bigint NOT NULL DEFAULT nextval('pgbench_graph_person_seq'),
    first_name text NOT NULL,
    last_name text NOT NULL,
    gender text NOT NULL DEFAULT 'female',
    city_id bigint NOT NULL,
    company_id integer NOT NULL DEFAULT 1,
    university_id integer NOT NULL DEFAULT 1,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_knows (
    src_id bigint NOT NULL,
    dst_id bigint NOT NULL,
    weight float8 NOT NULL DEFAULT 1.0,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_person_tags (
    person_id bigint NOT NULL,
    tag_id bigint NOT NULL
);

CREATE TABLE pgbench_graph_forums (
    id bigint NOT NULL DEFAULT nextval('pgbench_graph_forum_seq'),
    title text NOT NULL,
    moderator_id bigint NOT NULL,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_forum_members (
    forum_id bigint NOT NULL,
    person_id bigint NOT NULL,
    joined_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_messages (
    id bigint NOT NULL DEFAULT nextval('pgbench_graph_msg_seq'),
    creator_id bigint NOT NULL,
    forum_id bigint,
    parent_message_id bigint,
    country_id bigint NOT NULL DEFAULT 1,
    content text NOT NULL,
    length integer NOT NULL DEFAULT 24,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_likes (
    person_id bigint NOT NULL,
    message_id bigint NOT NULL,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_graph_message_tags (
    message_id bigint NOT NULL,
    tag_id bigint NOT NULL
);

-- 3. LDBC FinBench Financial Graph Entities & Multi-Edges
CREATE TABLE pgbench_fin_accounts (
    id bigint NOT NULL,
    owner_id bigint NOT NULL,
    owner_type text NOT NULL DEFAULT 'person',
    is_blocked boolean NOT NULL DEFAULT false,
    balance numeric(18,2) NOT NULL DEFAULT 10000.00,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_fin_transfers (
    id bigint NOT NULL DEFAULT nextval('pgbench_fin_transfer_seq'),
    src_acct bigint NOT NULL,
    dst_acct bigint NOT NULL,
    amount numeric(18,2) NOT NULL,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_fin_loans (
    id bigint NOT NULL DEFAULT nextval('pgbench_fin_loan_seq'),
    borrower_id bigint NOT NULL,
    acct_id bigint NOT NULL,
    loan_amount numeric(18,2) NOT NULL,
    balance numeric(18,2) NOT NULL,
    created_ts bigint NOT NULL
);

CREATE TABLE pgbench_fin_mediums (
    id bigint NOT NULL,
    type text NOT NULL,
    is_blocked boolean NOT NULL DEFAULT false,
    risk_score integer NOT NULL DEFAULT 10
);

CREATE TABLE pgbench_fin_sign_ins (
    medium_id bigint NOT NULL,
    acct_id bigint NOT NULL,
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

    INSERT INTO pgbench_graph_messages (creator_id, forum_id, country_id, content, length, created_ts)
    VALUES (p_src, 1 + (p_src % 20), 7 + (p_src % 10)::int, p_content, length(p_content), v_ts)
    RETURNING id INTO p_msg_id;
END;
$$;
