--
-- pgbench-graph-pgq: Property Graph Definitions for Native ISO SQL/PGQ (GRAPH_TABLE)
-- Defines snb_graph and finbench_graph using CREATE PROPERTY GRAPH.
--

SET client_min_messages = warning;

DROP PROPERTY GRAPH IF EXISTS finbench_graph CASCADE;
DROP PROPERTY GRAPH IF EXISTS snb_graph CASCADE;

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
