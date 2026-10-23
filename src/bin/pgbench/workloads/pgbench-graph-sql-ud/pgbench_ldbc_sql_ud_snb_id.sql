--
-- pgbench-graph (SQL_UD): LDBC SNB Interactive v2 Cascading Deletes (ID1 - ID8 / DEL1 - DEL8)
-- Executes recursive reply-subthread cascade deletions using WITH RECURSIVE ... UNION DISTINCT ON,
-- edge removals, and forum/member cleanup while coordinating with the causal dependency watermark.
--

\set del_type random(1, 4)
\set event_ts (1700200000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random(1, 200 * :scale)
\set p2_id ldbc_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 99)

\begin_dependent_write :event_ts CHANNEL 0

\if :del_type = 1
-- DEL6 / DEL7: Cascading Comment Sub-Thread Deletion via WITH RECURSIVE ... UNION DISTINCT ON (id)
-- Deletes a comment and all of its transitive reply descendants and their likes/tags
WITH RECURSIVE target_root AS (
    SELECT id
      FROM pgbench_graph_messages
     WHERE parent_message_id IS NOT NULL
       AND id >= 100000
     ORDER BY id DESC
     LIMIT 1
),
reply_tree AS (
    SELECT m.id AS id
      FROM pgbench_graph_messages m
      JOIN target_root tr ON tr.id = m.id
    UNION DISTINCT ON (id)
    SELECT child.id AS id
      FROM pgbench_graph_messages child
      JOIN reply_tree rt ON child.parent_message_id = rt.id
),
del_likes AS (
    DELETE FROM pgbench_graph_likes l
     USING reply_tree rt
     WHERE l.message_id = rt.id
),
del_tags AS (
    DELETE FROM pgbench_graph_message_tags mt
     USING reply_tree rt
     WHERE mt.message_id = rt.id
)
DELETE FROM pgbench_graph_messages m
 USING reply_tree rt
 WHERE m.id = rt.id;

\elif :del_type = 2
-- DEL2 / DEL3: Remove a Post/Comment Like
DELETE FROM pgbench_graph_likes
 WHERE (person_id, message_id) IN (
     SELECT person_id, message_id
       FROM pgbench_graph_likes
      WHERE person_id = :p1_id
      LIMIT 1
 );

\elif :del_type = 3
-- DEL5: Remove Forum Membership
DELETE FROM pgbench_graph_forum_members
 WHERE (forum_id, person_id) IN (
     SELECT forum_id, person_id
       FROM pgbench_graph_forum_members
      WHERE person_id = :p1_id
        AND forum_id > 20 * :scale
      LIMIT 1
 );

\else
-- DEL8: Remove Bidirectional Friendship Edge (for dynamically added edges)
DELETE FROM pgbench_graph_knows
 WHERE (src_id = :p1_id AND dst_id = :p2_id AND created_ts >= 1700100000000000)
    OR (src_id = :p2_id AND dst_id = :p1_id AND created_ts >= 1700100000000000);
\endif

\end_dependent_write :event_ts CHANNEL 0
