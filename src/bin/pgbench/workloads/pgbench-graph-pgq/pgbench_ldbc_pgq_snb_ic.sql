--
-- pgbench-graph-pgq: LDBC SNB Interactive Complex Reads (IC1-IC5, IC9, IC12, IC13)
-- Uses ISO SQL/PGQ GRAPH_TABLE pattern matching over snb_graph.
-- Discovers multi-hop friends & messages, pushes discovered IDs into shared
-- reservoirs via array-aware \reservoir_push, and chains into Short Reads via \call.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\set other_person_id ldbc_correlated_edge(:person_id, 200 * :scale, 10, 0.50, 77)
\set cutoff_ts 1700090000000000
\set window_start_ts 1700000000000000

-- IC1 & IC2: 1..2 hop friends deduplicated on-the-fly via UNION DISTINCT ON (friend_id) and GRAPH_TABLE
WITH RECURSIVE k_hops AS (
    SELECT g1.friend_id AS friend_id, 1 AS min_dist
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person WHERE src.id = :person_id)-[IS knows]->(f1 IS Person)
          COLUMNS (f1.id AS friend_id)
      ) g1
    UNION DISTINCT ON (friend_id)
    SELECT g2.friend_id AS friend_id, h.min_dist + 1 AS min_dist
      FROM k_hops h,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = h.friend_id)-[IS knows]->(f2 IS Person)
               COLUMNS (f2.id AS friend_id)
           ) g2
     WHERE h.min_dist < 2
       AND g2.friend_id <> :person_id
)
SELECT h.friend_id AS disc_person_id
  FROM k_hops h,
       GRAPH_TABLE (snb_graph
           MATCH (p IS Person WHERE p.id = h.friend_id)
           COLUMNS (p.id AS id, p.last_name AS last_name)
       ) p
 ORDER BY h.min_dist ASC, p.last_name ASC, p.id ASC
 LIMIT 10 \gset_col disc_person_id

\if reservoir_count('persons') >= 0
\reservoir_push persons :disc_person_id
\endif

SELECT g.disc_msg_id
  FROM GRAPH_TABLE (snb_graph
      MATCH (msg IS Message WHERE msg.created_ts <= :cutoff_ts)
      COLUMNS (msg.id AS disc_msg_id, msg.created_ts AS created_ts)
  ) g
 ORDER BY g.created_ts DESC
 LIMIT 10 \gset_col disc_msg_id

\if reservoir_count('messages') >= 0
\reservoir_push messages :disc_msg_id
\endif

-- IC3 & IC4: Popular tags among 1..2 hop friends' posts within time window via UNION DISTINCT ON (fid)
WITH RECURSIVE friends AS (
    SELECT g1.fid AS fid, 1 AS hop
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person WHERE src.id = :person_id)-[IS knows]->(f1 IS Person)
          COLUMNS (f1.id AS fid)
      ) g1
    UNION DISTINCT ON (fid)
    SELECT g2.fid AS fid, f.hop + 1 AS hop
      FROM friends f,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = f.fid)-[IS knows]->(f2 IS Person)
               COLUMNS (f2.id AS fid)
           ) g2
     WHERE f.hop < 2
       AND g2.fid <> :person_id
)
SELECT g.tag_name,
       count(*) AS post_count
  FROM friends f,
       GRAPH_TABLE (snb_graph
           MATCH (p IS Person WHERE p.id = f.fid)
                 <-[IS hasCreator]-(m IS Message WHERE m.parent_message_id IS NULL AND m.created_ts >= :window_start_ts AND m.created_ts <= :cutoff_ts)
                 -[IS hasTag]->(t IS Tag)
           COLUMNS (t.id AS tag_id, t.name AS tag_name)
       ) g
 GROUP BY g.tag_id, g.tag_name
 ORDER BY post_count DESC, g.tag_name ASC
 LIMIT 10;

-- IC5 & IC9: Forums joined and recent messages by friends-of-friends via GRAPH_TABLE
WITH fof AS (
    SELECT DISTINCT fid
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person WHERE src.id = :person_id)-[IS knows]->(f1 IS Person)-[IS knows]->(f2 IS Person)
          WHERE f2.id <> src.id
          COLUMNS (f2.id AS fid)
      )
)
SELECT gf.forum_id,
       gf.title,
       count(gm.msg_id) AS msg_count
  FROM fof,
       GRAPH_TABLE (snb_graph
           MATCH (f IS Forum)-[IS hasMember]->(p IS Person WHERE p.id = fof.fid)
           COLUMNS (f.id AS forum_id, f.title AS title, p.id AS member_id)
       ) gf
  LEFT JOIN GRAPH_TABLE (snb_graph
      MATCH (f IS Forum)-[IS containerOf]->(m IS Message)-[IS hasCreator]->(p IS Person)
      COLUMNS (f.id AS forum_id, m.id AS msg_id, p.id AS creator_id)
  ) gm ON gm.forum_id = gf.forum_id AND gm.creator_id = gf.member_id
 GROUP BY gf.forum_id, gf.title
 ORDER BY msg_count DESC, gf.forum_id ASC
 LIMIT 10;

-- IC12: Expert friend replies on posts tagged in a given tagclass
SELECT g.friend_id,
       count(DISTINCT g.cmt_id) AS reply_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (src IS Person WHERE src.id = :person_id)
            -[IS knows]->(friend IS Person)
            <-[IS hasCreator]-(cmt IS Message WHERE cmt.parent_message_id IS NOT NULL)
            -[IS replyOf]->(post IS Message)
            -[IS hasTag]->(t IS Tag WHERE t.tagclass_id <= 5)
      COLUMNS (friend.id AS friend_id, cmt.id AS cmt_id)
  ) g
 GROUP BY g.friend_id
 ORDER BY reply_count DESC, g.friend_id ASC
 LIMIT 10;

-- IC13: Bounded Shortest Path between :person_id and :other_person_id via WITH RECURSIVE ... UNION DISTINCT ON (node_id)
WITH RECURSIVE bfs AS (
    SELECT :person_id::bigint AS node_id,
           0 AS depth
    UNION DISTINCT ON (node_id)
    SELECT g.dst_id AS node_id,
           b.depth + 1 AS depth
      FROM bfs b,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = b.node_id)-[IS knows]->(dst IS Person)
               COLUMNS (dst.id AS dst_id)
           ) g
     WHERE b.depth < 4
)
SELECT COALESCE(MIN(depth), -1) AS shortest_path_len
  FROM bfs
 WHERE node_id = :other_person_id \gset

\validate_result (:shortest_path_len >= -1) true

-- Chain into LDBC SNB Short Reads (IS1-IS7) via \call
\call pgbench_ldbc_pgq_snb_is.sql person_id=:person_id
