--
-- pgbench-graph-pgq: LDBC SNB Interactive v2 Cascading Deletes (ID1 - ID8 / DEL1 - DEL8)
-- Executes recursive reply-subthread cascade deletions using GRAPH_TABLE step
-- expansions, edge removals, and forum/member cleanup while coordinating with
-- the causal dependency watermark.
--

\set del_type random(1, 4)
\set event_ts (1700200000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random(1, 200 * :scale)
\set p2_id ldbc_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 99)

\begin_dependent_write :event_ts CHANNEL 0

\if :del_type = 1
-- DEL6 / DEL7: Cascading Comment Sub-Thread Deletion via WITH RECURSIVE + GRAPH_TABLE
-- Deletes a comment and all of its transitive reply descendants and their likes/tags
WITH RECURSIVE target_root AS (
    SELECT g.id
      FROM GRAPH_TABLE (snb_graph
          MATCH (m IS Message WHERE m.parent_message_id IS NOT NULL AND m.id >= 100000)
          COLUMNS (m.id AS id)
      ) g
     ORDER BY g.id DESC
     LIMIT 1
),
reply_tree AS (
    SELECT tr.id AS id
      FROM target_root tr
    UNION DISTINCT ON (id)
    SELECT g.child_id AS id
      FROM reply_tree rt,
           GRAPH_TABLE (snb_graph
               MATCH (child IS Message)-[IS replyOf]->(parent IS Message WHERE parent.id = rt.id)
               COLUMNS (child.id AS child_id)
           ) g
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
-- DEL2 / DEL3: Remove a Post/Comment Like discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_likes
 WHERE (person_id, message_id) IN (
     SELECT g.person_id, g.message_id
       FROM GRAPH_TABLE (snb_graph
           MATCH (p IS Person WHERE p.id = :p1_id)-[IS likes]->(m IS Message)
           COLUMNS (p.id AS person_id, m.id AS message_id)
       ) g
      LIMIT 1
 );

\elif :del_type = 3
-- DEL5: Remove Forum Membership discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_forum_members
 WHERE (forum_id, person_id) IN (
     SELECT g.forum_id, g.person_id
       FROM GRAPH_TABLE (snb_graph
           MATCH (f IS Forum WHERE f.id > 20 * :scale)-[IS hasMember]->(p IS Person WHERE p.id = :p1_id)
           COLUMNS (f.id AS forum_id, p.id AS person_id)
       ) g
      LIMIT 1
 );

\else
-- DEL8: Remove Bidirectional Friendship Edge (for dynamically added edges)
DELETE FROM pgbench_graph_knows
 WHERE (src_id = :p1_id AND dst_id = :p2_id AND created_ts >= 1700100000000000)
    OR (src_id = :p2_id AND dst_id = :p1_id AND created_ts >= 1700100000000000);
\endif

\end_dependent_write :event_ts CHANNEL 0
