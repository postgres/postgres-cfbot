--
-- pgbench-graph-pgq (Unified LDBC Social Network & FinBench SQL/PGQ Workload Suite)
-- Schema DDL, Stored Procedures, and ISO SQL/PGQ Property Graphs
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP PROPERTY GRAPH IF EXISTS finbench_graph CASCADE;
DROP PROPERTY GRAPH IF EXISTS snb_graph CASCADE;

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

--
-- 4. ISO SQL/PGQ Property Graph Declarations
--    Uses explicit KEY (...) clauses so the property graphs can be created
--    before bulk parallel data load and primary key constraint creation.
--
CREATE PROPERTY GRAPH snb_graph
    NODE TABLES (
        pgbench_graph_places KEY (id)
            LABEL Place PROPERTIES ALL COLUMNS,
        pgbench_graph_tags KEY (id)
            LABEL Tag PROPERTIES ALL COLUMNS,
        pgbench_graph_persons KEY (id)
            LABEL Person PROPERTIES ALL COLUMNS,
        pgbench_graph_forums KEY (id)
            LABEL Forum PROPERTIES ALL COLUMNS,
        pgbench_graph_messages KEY (id)
            LABEL Message PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_graph_knows KEY (src_id, dst_id)
            SOURCE KEY (src_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (dst_id) REFERENCES pgbench_graph_persons (id)
            LABEL knows PROPERTIES ALL COLUMNS,
        pgbench_graph_person_tags KEY (person_id, tag_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasInterest PROPERTIES ALL COLUMNS,
        pgbench_graph_forum_members KEY (forum_id, person_id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (person_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasMember PROPERTIES ALL COLUMNS,
        pgbench_graph_likes KEY (person_id, message_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (message_id) REFERENCES pgbench_graph_messages (id)
            LABEL likes PROPERTIES ALL COLUMNS,
        pgbench_graph_message_tags KEY (message_id, tag_id)
            SOURCE KEY (message_id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasTag PROPERTIES ALL COLUMNS,
        pgbench_graph_messages AS msg_has_creator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (creator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasCreator PROPERTIES (id, creator_id, created_ts),
        pgbench_graph_messages AS msg_reply_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (parent_message_id) REFERENCES pgbench_graph_messages (id)
            LABEL replyOf PROPERTIES (id, parent_message_id, created_ts),
        pgbench_graph_messages AS msg_in_forum KEY (id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (id) REFERENCES pgbench_graph_messages (id)
            LABEL containerOf PROPERTIES (id, forum_id, created_ts),
        pgbench_graph_forums AS forum_has_moderator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (moderator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasModerator PROPERTIES (id, moderator_id, created_ts),
        pgbench_graph_persons AS person_located_in KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (city_id) REFERENCES pgbench_graph_places (id)
            LABEL isLocatedIn PROPERTIES (id, city_id),
        pgbench_graph_places AS place_part_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_places (id)
            DESTINATION KEY (part_of_id) REFERENCES pgbench_graph_places (id)
            LABEL isPartOf PROPERTIES (id, part_of_id)
    );

CREATE PROPERTY GRAPH finbench_graph
    NODE TABLES (
        pgbench_fin_accounts KEY (id)
            LABEL Account PROPERTIES ALL COLUMNS,
        pgbench_fin_mediums KEY (id)
            LABEL Medium PROPERTIES ALL COLUMNS,
        pgbench_fin_loans KEY (id)
            LABEL Loan PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_fin_transfers KEY (id)
            SOURCE KEY (src_acct) REFERENCES pgbench_fin_accounts (id)
            DESTINATION KEY (dst_acct) REFERENCES pgbench_fin_accounts (id)
            LABEL transfer PROPERTIES ALL COLUMNS,
        pgbench_fin_sign_ins KEY (medium_id, acct_id, created_ts)
            SOURCE KEY (medium_id) REFERENCES pgbench_fin_mediums (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL signIn PROPERTIES ALL COLUMNS,
        pgbench_fin_loans AS loan_deposit KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_fin_loans (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL deposit PROPERTIES (id, borrower_id, acct_id, loan_amount, balance, created_ts)
    );
