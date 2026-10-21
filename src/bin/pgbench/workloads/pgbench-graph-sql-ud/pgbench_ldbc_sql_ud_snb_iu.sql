--
-- pgbench-graph (SQL_UD): LDBC SNB Interactive Inserts (IU1 - IU8)
-- Uses \begin_dependent_write / \end_dependent_write (Multi-Writer Min-Watermark
-- Time Gatekeeper) and pushes newly created entity IDs into shared reservoirs.
--

\set op_type random(1, 8)
\set event_ts (1700100000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random_zipfian(1, 200 * :scale, 1.2)
\set p2_id ldbc_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 42)
\set forum_id random(1, 20 * :scale)
\set tag_id random(1, 100)

\begin_dependent_write :event_ts CHANNEL 0

\if :op_type = 1
-- IU1: Add Person + Person Tags
INSERT INTO pgbench_graph_persons (first_name, last_name, gender, city_id, company_id, university_id, created_ts)
VALUES ('NewPerson', 'Stream', 'female', 17 + (:p1_id % 50), 1 + (:p1_id % 20), 1 + (:p1_id % 15), :event_ts)
RETURNING id AS new_person_id \gset
INSERT INTO pgbench_graph_person_tags (person_id, tag_id)
VALUES (:new_person_id, :tag_id)
ON CONFLICT DO NOTHING;
\if reservoir_count('persons') >= 0
\reservoir_push persons :new_person_id
\endif

\elif :op_type = 2
-- IU2: Add Post Like
INSERT INTO pgbench_graph_likes (person_id, message_id, created_ts)
VALUES (:p1_id, 1 + ((:p2_id - 1) * 2), :event_ts)
ON CONFLICT (person_id, message_id) DO UPDATE SET created_ts = EXCLUDED.created_ts;

\elif :op_type = 3
-- IU3: Add Comment Like
INSERT INTO pgbench_graph_likes (person_id, message_id, created_ts)
VALUES (:p1_id, (:p2_id - 1) * 2 + 2, :event_ts)
ON CONFLICT (person_id, message_id) DO UPDATE SET created_ts = EXCLUDED.created_ts;

\elif :op_type = 4
-- IU4: Add Forum
INSERT INTO pgbench_graph_forums (title, moderator_id, created_ts)
VALUES ('StreamForum_' || :event_ts::text, :p1_id, :event_ts)
RETURNING id AS new_forum_id \gset
INSERT INTO pgbench_graph_forum_members (forum_id, person_id, joined_ts)
VALUES (:new_forum_id, :p1_id, :event_ts)
ON CONFLICT DO NOTHING;

\elif :op_type = 5
-- IU5: Add Forum Membership
INSERT INTO pgbench_graph_forum_members (forum_id, person_id, joined_ts)
VALUES (:forum_id, :p1_id, :event_ts)
ON CONFLICT (forum_id, person_id) DO UPDATE SET joined_ts = EXCLUDED.joined_ts;

\elif :op_type = 6
-- IU6: Add Post + Message Tag
INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:p1_id, :forum_id, NULL, 7 + (:p1_id % 10), 'IU6 Stream Post ' || :event_ts::text, 28, :event_ts)
RETURNING id AS new_msg_id \gset
INSERT INTO pgbench_graph_message_tags (message_id, tag_id)
VALUES (:new_msg_id, :tag_id)
ON CONFLICT DO NOTHING;
\if reservoir_count('messages') >= 0
\reservoir_push messages :new_msg_id
\endif

\elif :op_type = 7
-- IU7: Add Comment Reply + Message Tag
INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:p1_id, :forum_id, 1 + ((:p2_id - 1) * 2), 7 + (:p1_id % 10), 'IU7 Stream Reply ' || :event_ts::text, 29, :event_ts)
RETURNING id AS new_msg_id \gset
INSERT INTO pgbench_graph_message_tags (message_id, tag_id)
VALUES (:new_msg_id, :tag_id)
ON CONFLICT DO NOTHING;
\if reservoir_count('messages') >= 0
\reservoir_push messages :new_msg_id
\endif

\else
-- IU8: Add Bidirectional Friendship (KNOWS)
INSERT INTO pgbench_graph_knows (src_id, dst_id, weight, created_ts)
VALUES (:p1_id, :p2_id, 1.0, :event_ts),
       (:p2_id, :p1_id, 1.0, :event_ts)
ON CONFLICT (src_id, dst_id) DO UPDATE SET created_ts = EXCLUDED.created_ts;
\endif

\end_dependent_write :event_ts CHANNEL 0
