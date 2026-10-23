--
-- pgbench-graph (SQL_UD): LDBC SNB Interactive Complex Reads (IC1-IC5, IC9, IC12, IC13)
-- Uses WITH RECURSIVE ... UNION DISTINCT ON (...) supported by the
-- postsql/postgres one-with-everything branch to prune duplicate vertices
-- during BFS expansion and eliminate post-recursion GROUP BY deduplication.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

\set other_person_id ldbc_correlated_edge(:person_id, 200 * :scale, 10, 0.50, 77)
\set cutoff_ts 1700090000000000
\set window_start_ts 1700000000000000

-- IC1 & IC2: 1..2 hop friends deduplicated on-the-fly via UNION DISTINCT ON (friend_id)
WITH RECURSIVE k_hops AS (
    SELECT k.dst_id AS friend_id, 1 AS min_dist
      FROM pgbench_graph_knows k
     WHERE k.src_id = :person_id
    UNION DISTINCT ON (friend_id)
    SELECT k2.dst_id AS friend_id, h.min_dist + 1 AS min_dist
      FROM k_hops h
      JOIN pgbench_graph_knows k2 ON k2.src_id = h.friend_id
     WHERE h.min_dist < 2
       AND k2.dst_id <> :person_id
)
SELECT h.friend_id AS disc_person_id
  FROM k_hops h
  JOIN pgbench_graph_persons p ON p.id = h.friend_id
 ORDER BY h.min_dist ASC, p.last_name ASC, p.id ASC
 LIMIT 10 \gset_col disc_person_id

\if reservoir_count('persons') >= 0
\reservoir_push persons :disc_person_id
\endif

SELECT msg.id AS disc_msg_id
  FROM pgbench_graph_messages msg
 WHERE msg.created_ts <= :cutoff_ts
 ORDER BY msg.created_ts DESC
 LIMIT 10 \gset_col disc_msg_id

\if reservoir_count('messages') >= 0
\reservoir_push messages :disc_msg_id
\endif

-- IC3 & IC4: Popular tags among 1..2 hop friends' posts within time window via UNION DISTINCT ON (fid)
WITH RECURSIVE friends AS (
    SELECT k.dst_id AS fid, 1 AS hop
      FROM pgbench_graph_knows k
     WHERE k.src_id = :person_id
    UNION DISTINCT ON (fid)
    SELECT k2.dst_id AS fid, f.hop + 1 AS hop
      FROM friends f
      JOIN pgbench_graph_knows k2 ON k2.src_id = f.fid
     WHERE f.hop < 2
       AND k2.dst_id <> :person_id
)
SELECT t.name AS tag_name,
       count(*) AS post_count
  FROM friends f
  JOIN pgbench_graph_messages m ON m.creator_id = f.fid
  JOIN pgbench_graph_message_tags mt ON mt.message_id = m.id
  JOIN pgbench_graph_tags t ON t.id = mt.tag_id
 WHERE m.parent_message_id IS NULL
   AND m.created_ts BETWEEN :window_start_ts AND :cutoff_ts
 GROUP BY t.id, t.name
 ORDER BY post_count DESC, t.name ASC
 LIMIT 10;

-- IC5 & IC9: Forums joined and recent messages by friends-of-friends
WITH fof AS (
    SELECT DISTINCT k2.dst_id AS fid
      FROM pgbench_graph_knows k1
      JOIN pgbench_graph_knows k2 ON k2.src_id = k1.dst_id
     WHERE k1.src_id = :person_id
       AND k2.dst_id <> :person_id
)
SELECT f.id AS forum_id,
       f.title,
       count(m.id) AS msg_count
  FROM fof
  JOIN pgbench_graph_forum_members fm ON fm.person_id = fof.fid
  JOIN pgbench_graph_forums f ON f.id = fm.forum_id
  LEFT JOIN pgbench_graph_messages m ON m.forum_id = f.id AND m.creator_id = fof.fid
 GROUP BY f.id, f.title
 ORDER BY msg_count DESC, f.id ASC
 LIMIT 10;

-- IC12: Expert friend replies on posts tagged in a given tagclass
SELECT k.dst_id AS friend_id,
       count(DISTINCT cmt.id) AS reply_count
  FROM pgbench_graph_knows k
  JOIN pgbench_graph_messages cmt ON cmt.creator_id = k.dst_id AND cmt.parent_message_id IS NOT NULL
  JOIN pgbench_graph_messages post ON post.id = cmt.parent_message_id
  JOIN pgbench_graph_message_tags mt ON mt.message_id = post.id
  JOIN pgbench_graph_tags t ON t.id = mt.tag_id
 WHERE k.src_id = :person_id
   AND t.tagclass_id <= 5
 GROUP BY k.dst_id
 ORDER BY reply_count DESC, k.dst_id ASC
 LIMIT 10;

-- IC13: Bounded Shortest Path between :person_id and :other_person_id via WITH RECURSIVE ... UNION DISTINCT ON (node_id)
WITH RECURSIVE bfs AS (
    SELECT :person_id::bigint AS node_id,
           0 AS depth
    UNION DISTINCT ON (node_id)
    SELECT k.dst_id AS node_id,
           b.depth + 1 AS depth
      FROM bfs b
      JOIN pgbench_graph_knows k ON k.src_id = b.node_id
     WHERE b.depth < 4
)
SELECT COALESCE(MIN(depth), -1) AS shortest_path_len
  FROM bfs
 WHERE node_id = :other_person_id \gset

\validate_result (:shortest_path_len >= -1) true

-- Chain into LDBC SNB Short Reads (IS1-IS7) via \call
\call pgbench_ldbc_sql_ud_snb_is.sql person_id=:person_id
