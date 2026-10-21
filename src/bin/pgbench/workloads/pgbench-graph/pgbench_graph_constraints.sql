--
-- pgbench-graph (Unified LDBC Social Network & FinBench Graph Workload Suite)
-- Primary Keys, Adjacency Indexes, and Statistics
--

SET client_min_messages = warning;

-- Deduplicate any cross-worker bidirectional edges before adding primary keys
DELETE FROM pgbench_graph_knows a
 USING pgbench_graph_knows b
 WHERE a.ctid < b.ctid
   AND a.src_id = b.src_id
   AND a.dst_id = b.dst_id;

DELETE FROM pgbench_graph_person_tags a
 USING pgbench_graph_person_tags b
 WHERE a.ctid < b.ctid
   AND a.person_id = b.person_id
   AND a.tag_id = b.tag_id;

DELETE FROM pgbench_graph_forum_members a
 USING pgbench_graph_forum_members b
 WHERE a.ctid < b.ctid
   AND a.forum_id = b.forum_id
   AND a.person_id = b.person_id;

DELETE FROM pgbench_graph_likes a
 USING pgbench_graph_likes b
 WHERE a.ctid < b.ctid
   AND a.person_id = b.person_id
   AND a.message_id = b.message_id;

DELETE FROM pgbench_graph_message_tags a
 USING pgbench_graph_message_tags b
 WHERE a.ctid < b.ctid
   AND a.message_id = b.message_id
   AND a.tag_id = b.tag_id;

ALTER TABLE pgbench_graph_places
    ADD CONSTRAINT pgbench_graph_places_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_tags
    ADD CONSTRAINT pgbench_graph_tags_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_persons
    ADD CONSTRAINT pgbench_graph_persons_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_knows
    ADD CONSTRAINT pgbench_graph_knows_pkey PRIMARY KEY (src_id, dst_id);

ALTER TABLE pgbench_graph_person_tags
    ADD CONSTRAINT pgbench_graph_person_tags_pkey PRIMARY KEY (person_id, tag_id);

ALTER TABLE pgbench_graph_forums
    ADD CONSTRAINT pgbench_graph_forums_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_forum_members
    ADD CONSTRAINT pgbench_graph_forum_members_pkey PRIMARY KEY (forum_id, person_id);

ALTER TABLE pgbench_graph_messages
    ADD CONSTRAINT pgbench_graph_messages_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_likes
    ADD CONSTRAINT pgbench_graph_likes_pkey PRIMARY KEY (person_id, message_id);

ALTER TABLE pgbench_graph_message_tags
    ADD CONSTRAINT pgbench_graph_message_tags_pkey PRIMARY KEY (message_id, tag_id);

ALTER TABLE pgbench_fin_accounts
    ADD CONSTRAINT pgbench_fin_accounts_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_fin_transfers
    ADD CONSTRAINT pgbench_fin_transfers_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_fin_loans
    ADD CONSTRAINT pgbench_fin_loans_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_fin_mediums
    ADD CONSTRAINT pgbench_fin_mediums_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_fin_sign_ins
    ADD CONSTRAINT pgbench_fin_sign_ins_pkey PRIMARY KEY (medium_id, acct_id, created_ts);

-- Adjacency & Temporal Indexes
CREATE INDEX pgbench_graph_knows_dst_src_idx
    ON pgbench_graph_knows (dst_id, src_id);

CREATE INDEX pgbench_graph_messages_creator_ts_idx
    ON pgbench_graph_messages (creator_id, created_ts DESC);

CREATE INDEX pgbench_graph_messages_parent_idx
    ON pgbench_graph_messages (parent_message_id)
    WHERE parent_message_id IS NOT NULL;

CREATE INDEX pgbench_graph_messages_forum_ts_idx
    ON pgbench_graph_messages (forum_id, created_ts DESC);

CREATE INDEX pgbench_graph_likes_msg_idx
    ON pgbench_graph_likes (message_id, person_id);

CREATE INDEX pgbench_graph_msg_tags_tag_idx
    ON pgbench_graph_message_tags (tag_id, message_id);

CREATE INDEX pgbench_graph_person_tags_tag_idx
    ON pgbench_graph_person_tags (tag_id, person_id);

CREATE INDEX pgbench_fin_transfers_src_ts_idx
    ON pgbench_fin_transfers (src_acct, created_ts, dst_acct);

CREATE INDEX pgbench_fin_transfers_dst_ts_idx
    ON pgbench_fin_transfers (dst_acct, created_ts, src_acct);

CREATE INDEX pgbench_fin_loans_borrower_idx
    ON pgbench_fin_loans (borrower_id, acct_id);

CREATE INDEX pgbench_graph_forum_members_person_idx
    ON pgbench_graph_forum_members (person_id, forum_id);

CREATE INDEX pgbench_fin_sign_ins_acct_idx
    ON pgbench_fin_sign_ins (acct_id, medium_id, created_ts);

CREATE INDEX pgbench_graph_persons_city_idx
    ON pgbench_graph_persons (city_id, id);

CREATE INDEX pgbench_graph_places_part_of_idx
    ON pgbench_graph_places (part_of_id, type, id);

CREATE INDEX pgbench_graph_messages_created_ts_idx
    ON pgbench_graph_messages (created_ts DESC, id);

CREATE INDEX pgbench_graph_forums_moderator_idx
    ON pgbench_graph_forums (moderator_id, id);

CREATE INDEX pgbench_graph_tags_tagclass_idx
    ON pgbench_graph_tags (tagclass_id, id);

ANALYZE pgbench_graph_places;
ANALYZE pgbench_graph_tags;
ANALYZE pgbench_graph_persons;
ANALYZE pgbench_graph_knows;
ANALYZE pgbench_graph_person_tags;
ANALYZE pgbench_graph_forums;
ANALYZE pgbench_graph_forum_members;
ANALYZE pgbench_graph_messages;
ANALYZE pgbench_graph_likes;
ANALYZE pgbench_graph_message_tags;
ANALYZE pgbench_fin_accounts;
ANALYZE pgbench_fin_transfers;
ANALYZE pgbench_fin_loans;
ANALYZE pgbench_fin_mediums;
ANALYZE pgbench_fin_sign_ins;
