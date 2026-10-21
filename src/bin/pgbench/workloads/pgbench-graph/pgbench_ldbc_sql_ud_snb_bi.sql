--
-- pgbench-graph (SQL_UD): LDBC SNB Business Intelligence (BI) Phased Workload Suite
-- Uses WITH RECURSIVE ... UNION DISTINCT ON (...) supported by postsql/postgres
-- one-with-everything for BI-9 reply-tree expansion, BI-19 weighted shortest paths,
-- and Phase 2 cascading reply deletion.
--

\set cutoff_ts 1700090000000000
\set window1_start 1700000000000000
\set window1_end   1700040000000000
\set window2_end   1700080000000000
\set country_id (7 + (:client_id % 10))
\set city1_id (17 + (:client_id % 25))
\set city2_id (42 + (:client_id % 25))

-- ============================================================================
-- PHASE 1: BI Power Run (Analytical Queries BI-1, BI-2, BI-4, BI-9, BI-11, BI-19)
-- ============================================================================

-- BI-1: Posting Summary Report by message type (Post vs Comment) and length category
WITH msg_base AS (
    SELECT (parent_message_id IS NOT NULL) AS is_comment,
           CASE
               WHEN length < 40 THEN 0
               WHEN length < 80 THEN 1
               WHEN length < 160 THEN 2
               ELSE 3
           END AS length_cat,
           length
      FROM pgbench_graph_messages
     WHERE created_ts <= :cutoff_ts
),
totals AS (
    SELECT GREATEST(count(*), 1) AS total_msgs FROM msg_base
)
SELECT count(*) AS bi1_groups,
       COALESCE(sum(mb_cnt), 0) AS bi1_total_msgs
  FROM (
      SELECT mb.is_comment,
             mb.length_cat,
             count(*) AS mb_cnt,
             round(avg(mb.length)::numeric, 2) AS avg_len,
             sum(mb.length) AS sum_len,
             round((count(*)::numeric / max(t.total_msgs)), 4) AS pct_msgs
        FROM msg_base mb
        CROSS JOIN totals t
       GROUP BY mb.is_comment, mb.length_cat
  ) q \gset

\validate_result (:bi1_groups >= 1 and :bi1_total_msgs >= 1) true

-- BI-2: Tag Evolution across two contiguous time windows
SELECT count(*) AS bi2_rows
  FROM (
      SELECT t.name AS tag_name,
             count(*) FILTER (WHERE m.created_ts >= :window1_start AND m.created_ts < :window1_end) AS cnt_w1,
             count(*) FILTER (WHERE m.created_ts >= :window1_end AND m.created_ts < :window2_end) AS cnt_w2,
             abs(
                 count(*) FILTER (WHERE m.created_ts >= :window1_start AND m.created_ts < :window1_end) -
                 count(*) FILTER (WHERE m.created_ts >= :window1_end AND m.created_ts < :window2_end)
             ) AS diff
        FROM pgbench_graph_tags t
        LEFT JOIN pgbench_graph_message_tags mt ON mt.tag_id = t.id
        LEFT JOIN pgbench_graph_messages m ON m.id = mt.message_id
        WHERE t.tagclass_id <= 5
       GROUP BY t.id, t.name
       ORDER BY diff DESC, t.name ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi2_rows >= 1) true

-- BI-4: Top Message Creators in Country Forums
WITH top_forums AS (
    SELECT f.id AS forum_id
      FROM pgbench_graph_forums f
      JOIN pgbench_graph_persons mod_p ON mod_p.id = f.moderator_id
      JOIN pgbench_graph_places city ON city.id = mod_p.city_id
     WHERE city.part_of_id = :country_id
     ORDER BY f.id ASC
     LIMIT 10
)
SELECT count(*) AS bi4_rows
  FROM (
      SELECT p.id AS person_id,
             p.first_name,
             p.last_name,
             count(m.id) AS message_count
        FROM top_forums tf
        JOIN pgbench_graph_forum_members fm ON fm.forum_id = tf.forum_id
        JOIN pgbench_graph_persons p ON p.id = fm.person_id
        LEFT JOIN pgbench_graph_messages m
          ON m.creator_id = p.id
         AND m.forum_id = tf.forum_id
       GROUP BY p.id, p.first_name, p.last_name
       ORDER BY message_count DESC, p.id ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi4_rows >= 0) true

-- BI-9: Top Thread Initiators via WITH RECURSIVE ... UNION DISTINCT ON (msg_id)
WITH RECURSIVE thread_tree AS (
    SELECT post.id AS root_post_id,
           post.creator_id AS root_author_id,
           post.id AS msg_id
      FROM pgbench_graph_messages post
     WHERE post.parent_message_id IS NULL
       AND post.created_ts BETWEEN :window1_start AND :cutoff_ts
    UNION DISTINCT ON (msg_id)
    SELECT tt.root_post_id AS root_post_id,
           tt.root_author_id AS root_author_id,
           reply.id AS msg_id
      FROM thread_tree tt
      JOIN pgbench_graph_messages reply ON reply.parent_message_id = tt.msg_id
     WHERE reply.created_ts BETWEEN :window1_start AND :cutoff_ts
)
SELECT count(*) AS bi9_rows
  FROM (
      SELECT p.id AS person_id,
             count(DISTINCT tt.root_post_id) AS thread_count,
             count(tt.msg_id) AS message_count
        FROM pgbench_graph_persons p
        JOIN thread_tree tt ON tt.root_author_id = p.id
       GROUP BY p.id
       ORDER BY message_count DESC, p.id ASC
       LIMIT 15
  ) q \gset

\validate_result (:bi9_rows >= 1) true

-- BI-11: Friend Triangles within a Country
SELECT count(*) AS bi11_triangles
  FROM pgbench_graph_knows k1
  JOIN pgbench_graph_knows k2 ON k2.src_id = k1.dst_id AND k2.dst_id > k1.dst_id
  JOIN pgbench_graph_knows k3 ON k3.src_id = k1.src_id AND k3.dst_id = k2.dst_id
  JOIN pgbench_graph_persons p1 ON p1.id = k1.src_id
  JOIN pgbench_graph_persons p2 ON p2.id = k1.dst_id
  JOIN pgbench_graph_persons p3 ON p3.id = k2.dst_id
  JOIN pgbench_graph_places c1 ON c1.id = p1.city_id
  JOIN pgbench_graph_places c2 ON c2.id = p2.city_id AND c2.part_of_id = c1.part_of_id
  JOIN pgbench_graph_places c3 ON c3.id = p3.city_id AND c3.part_of_id = c1.part_of_id
 WHERE k1.src_id < k1.dst_id \gset

\validate_result (:bi11_triangles >= 0) true

-- BI-19: Interaction-Weighted Shortest Path Between Two Cities via UNION DISTINCT ON (start_id, curr_id ORDER BY total_weight ASC)
WITH RECURSIVE city_paths AS (
    SELECT p.id AS start_id,
           p.id AS curr_id,
           0.0::float8 AS total_weight,
           0 AS hops
      FROM pgbench_graph_persons p
     WHERE p.city_id = :city1_id
    UNION DISTINCT ON (start_id, curr_id ORDER BY total_weight ASC)
    SELECT cp.start_id AS start_id,
           k.dst_id AS curr_id,
           cp.total_weight + k.weight AS total_weight,
           cp.hops + 1 AS hops
      FROM city_paths cp
      JOIN pgbench_graph_knows k ON k.src_id = cp.curr_id
     WHERE cp.hops < 3
)
SELECT COALESCE(min(total_weight), 0.0) AS bi19_min_weight
  FROM city_paths cp
  JOIN pgbench_graph_persons dst_p ON dst_p.id = cp.curr_id
 WHERE dst_p.city_id = :city2_id \gset

\validate_result (:bi19_min_weight >= 0.0) true

-- ============================================================================
-- BARRIER 1: Synchronize all BI clients before Micro-Batch Refresh
-- ============================================================================
\phase_barrier bi_microbatch

-- ============================================================================
-- PHASE 2: BI Micro-Batch Update (Insert Batch + Cascading Reply Delete Batch)
-- ============================================================================
\set batch_ts (1700050000000000 + :client_id * 1000)
\set batch_creator (1 + (:client_id % (200 * :scale)))
\set batch_friend ldbc_correlated_edge(:batch_creator, 200 * :scale, 10, 0.75, 55)

INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:batch_creator, 1 + (:batch_creator % (20 * :scale)), NULL, :country_id, 'BI Microbatch Post', 18, :batch_ts)
RETURNING id AS bi_post_id \gset

INSERT INTO pgbench_graph_messages (creator_id, forum_id, parent_message_id, country_id, content, length, created_ts)
VALUES (:batch_friend, 1 + (:batch_creator % (20 * :scale)), :bi_post_id, :country_id, 'BI Microbatch Reply', 19, :batch_ts + 100)
RETURNING id AS bi_reply_id \gset

INSERT INTO pgbench_graph_message_tags (message_id, tag_id)
VALUES (:bi_post_id, 1 + (:client_id % 100)),
       (:bi_reply_id, 2 + (:client_id % 99))
ON CONFLICT DO NOTHING;

-- Delete temporary reply sub-thread in micro-batch cleanup via UNION DISTINCT ON (id)
WITH RECURSIVE del_tree AS (
    SELECT id AS id FROM pgbench_graph_messages WHERE id = :bi_reply_id
    UNION DISTINCT ON (id)
    SELECT m.id AS id FROM pgbench_graph_messages m JOIN del_tree dt ON m.parent_message_id = dt.id
),
del_mt AS (
    DELETE FROM pgbench_graph_message_tags mt USING del_tree dt WHERE mt.message_id = dt.id
)
DELETE FROM pgbench_graph_messages m USING del_tree dt WHERE m.id = dt.id;

-- ============================================================================
-- BARRIER 2: Synchronize all BI clients before Multi-Stream Throughput Run
-- ============================================================================
\phase_barrier bi_throughput

-- ============================================================================
-- PHASE 3: BI Throughput Run (Concurrent Post-Refresh Analytical Verification)
-- ============================================================================
SELECT count(*) AS post_refresh_msgs
  FROM pgbench_graph_messages
 WHERE created_ts <= :cutoff_ts \gset

\validate_result (:post_refresh_msgs > :bi1_total_msgs) true
