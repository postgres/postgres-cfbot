--
-- pgbench-graph-pgq: LDBC SNB Interactive Short Reads (IS1 - IS7)
-- Uses ISO SQL/PGQ GRAPH_TABLE pattern matching over snb_graph.
-- Can be invoked standalone (sampling from --reservoir=persons / --reservoir=messages)
-- or chained via \call from pgbench_ldbc_pgq_snb_ic.sql with person_id=... message_id=...
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
SELECT g.first_name,
       g.last_name,
       g.gender,
       g.city_name,
       g.created_ts
  FROM GRAPH_TABLE (snb_graph
      MATCH (p IS Person WHERE p.id = :person_id)-[IS isLocatedIn]->(pl IS Place)
      COLUMNS (
          p.first_name AS first_name,
          p.last_name AS last_name,
          p.gender AS gender,
          pl.name AS city_name,
          p.created_ts AS created_ts
      )
  ) g;

-- IS2: Recent messages of a person & original root post info
SELECT g.msg_id,
       g.content,
       g.created_ts,
       COALESCE(parent_g.post_id, g.msg_id) AS post_id,
       COALESCE(parent_g.post_author_id, g.creator_id) AS post_author_id
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message)-[IS hasCreator]->(p IS Person WHERE p.id = :person_id)
      COLUMNS (
          m.id AS msg_id,
          m.content AS content,
          m.created_ts AS created_ts,
          m.creator_id AS creator_id
      )
  ) g
  LEFT JOIN GRAPH_TABLE (snb_graph
      MATCH (child IS Message)-[IS replyOf]->(root_p IS Message)
      COLUMNS (
          child.id AS child_id,
          root_p.id AS post_id,
          root_p.creator_id AS post_author_id
      )
  ) parent_g ON parent_g.child_id = g.msg_id
 ORDER BY g.created_ts DESC, g.msg_id DESC
 LIMIT 10;

-- IS3: Friends of a person
SELECT g.friend_id,
       g.first_name,
       g.last_name,
       g.friendship_ts
  FROM GRAPH_TABLE (snb_graph
      MATCH (src IS Person WHERE src.id = :person_id)-[k IS knows]->(friend IS Person)
      COLUMNS (
          friend.id AS friend_id,
          friend.first_name AS first_name,
          friend.last_name AS last_name,
          k.created_ts AS friendship_ts
      )
  ) g
 ORDER BY g.friendship_ts DESC, g.friend_id ASC
 LIMIT 20;

-- IS4 & IS5: Content and creator of a message
SELECT g.content,
       g.created_ts,
       g.author_id,
       g.first_name,
       g.last_name
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message WHERE m.id = :message_id)-[IS hasCreator]->(p IS Person)
      COLUMNS (
          m.content AS content,
          m.created_ts AS created_ts,
          p.id AS author_id,
          p.first_name AS first_name,
          p.last_name AS last_name
      )
  ) g;

-- IS6: Forum of a message and its moderator
SELECT g.forum_id,
       g.forum_title,
       g.moderator_id,
       g.first_name,
       g.last_name
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message WHERE m.id = :message_id)<-[IS containerOf]-(f IS Forum)-[IS hasModerator]->(mod_p IS Person)
      COLUMNS (
          f.id AS forum_id,
          f.title AS forum_title,
          mod_p.id AS moderator_id,
          mod_p.first_name AS first_name,
          mod_p.last_name AS last_name
      )
  ) g;

-- IS7: Replies of a message and whether the message author knows the reply author
SELECT g.comment_id,
       g.content,
       g.created_ts,
       g.reply_author_id,
       g.first_name,
       g.last_name,
       EXISTS (
           SELECT 1
             FROM GRAPH_TABLE (snb_graph
                 MATCH (a IS Person WHERE a.id = g.msg_author_id)-[IS knows]->(b IS Person WHERE b.id = g.reply_author_id)
                 COLUMNS (1 AS one)
             ) k
       ) AS knows_author
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message WHERE m.id = :message_id)<-[IS replyOf]-(cmt IS Message)-[IS hasCreator]->(rp IS Person)
      COLUMNS (
          m.creator_id AS msg_author_id,
          cmt.id AS comment_id,
          cmt.content AS content,
          cmt.created_ts AS created_ts,
          rp.id AS reply_author_id,
          rp.first_name AS first_name,
          rp.last_name AS last_name
      )
  ) g
 ORDER BY g.created_ts DESC, g.reply_author_id ASC
 LIMIT 15;
