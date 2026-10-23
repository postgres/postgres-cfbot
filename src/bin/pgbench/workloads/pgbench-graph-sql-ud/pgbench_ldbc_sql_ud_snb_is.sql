--
-- pgbench-graph (SQL_UD): LDBC SNB Interactive Short Reads (IS1 - IS7)
-- Can be invoked standalone (sampling from --reservoir=persons / --reservoir=messages)
-- or chained via \call from pgbench_ldbc_sql_ud_snb_ic.sql with person_id=... message_id=...
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\if reservoir_count('messages') >= 0
\reservoir_sample messages message_id random(1, 400 * :scale)
\else
\set message_id random(1, 400 * :scale)
\endif

-- IS1: Profile of a person (first name, last name, gender, city name, creation time)
SELECT p.first_name,
       p.last_name,
       p.gender,
       pl.name AS city_name,
       p.created_ts
  FROM pgbench_graph_persons p
  JOIN pgbench_graph_places pl ON pl.id = p.city_id
 WHERE p.id = :person_id;

-- IS2: Recent messages of a person & original root post info
SELECT m.id AS msg_id,
       m.content,
       m.created_ts,
       COALESCE(parent_m.id, m.id) AS post_id,
       COALESCE(parent_m.creator_id, m.creator_id) AS post_author_id
  FROM pgbench_graph_messages m
  LEFT JOIN pgbench_graph_messages parent_m ON parent_m.id = m.parent_message_id
 WHERE m.creator_id = :person_id
 ORDER BY m.created_ts DESC, m.id DESC
 LIMIT 10;

-- IS3: Friends of a person
SELECT p.id AS friend_id,
       p.first_name,
       p.last_name,
       k.created_ts AS friendship_ts
  FROM pgbench_graph_knows k
  JOIN pgbench_graph_persons p ON p.id = k.dst_id
 WHERE k.src_id = :person_id
 ORDER BY k.created_ts DESC, p.id ASC
 LIMIT 20;

-- IS4 & IS5: Content and creator of a message
SELECT m.content,
       m.created_ts,
       p.id AS author_id,
       p.first_name,
       p.last_name
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_persons p ON p.id = m.creator_id
 WHERE m.id = :message_id;

-- IS6: Forum of a message and its moderator
SELECT f.id AS forum_id,
       f.title AS forum_title,
       mod_p.id AS moderator_id,
       mod_p.first_name,
       mod_p.last_name
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_forums f ON f.id = m.forum_id
  JOIN pgbench_graph_persons mod_p ON mod_p.id = f.moderator_id
 WHERE m.id = :message_id;

-- IS7: Replies of a message and whether the message author knows the reply author
SELECT cmt.id AS comment_id,
       cmt.content,
       cmt.created_ts,
       rp.id AS reply_author_id,
       rp.first_name,
       rp.last_name,
       EXISTS (
           SELECT 1
             FROM pgbench_graph_knows k
            WHERE k.src_id = m.creator_id
              AND k.dst_id = rp.id
       ) AS knows_author
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = m.id
  JOIN pgbench_graph_persons rp ON rp.id = cmt.creator_id
 WHERE m.id = :message_id
 ORDER BY cmt.created_ts DESC, rp.id ASC
 LIMIT 15;
