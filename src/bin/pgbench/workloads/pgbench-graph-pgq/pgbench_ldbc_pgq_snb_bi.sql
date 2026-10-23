--
-- pgbench-graph-pgq: LDBC SNB Business Intelligence (BI) Phased Workload Suite
-- Uses ISO SQL/PGQ GRAPH_TABLE pattern matching over snb_graph.
-- Orchestrates sequential phases across worker clients using \phase_barrier:
--   Phase 1 (Power Run): BI-1, BI-2, BI-4, BI-9, BI-11, BI-19 analytical queries
--   \phase_barrier bi_microbatch
--   Phase 2 (Micro-Batch Update): Partitioned inserts + cascading sub-thread delete
--   \phase_barrier bi_throughput
--   Phase 3 (Throughput Run): Concurrent multi-stream BI analytical queries
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
    SELECT (g.parent_message_id IS NOT NULL) AS is_comment,
           CASE
               WHEN g.length < 40 THEN 0
               WHEN g.length < 80 THEN 1
               WHEN g.length < 160 THEN 2
               ELSE 3
           END AS length_cat,
           g.length
      FROM GRAPH_TABLE (snb_graph
          MATCH (m IS Message WHERE m.created_ts <= :cutoff_ts)
          COLUMNS (m.parent_message_id AS parent_message_id, m.length AS length)
      ) g
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

-- BI-2: Tag Evolution across two contiguous time windows via GRAPH_TABLE
SELECT count(*) AS bi2_rows
  FROM (
      SELECT gt.tag_name,
             count(*) FILTER (WHERE gm.created_ts >= :window1_start AND gm.created_ts < :window1_end) AS cnt_w1,
             count(*) FILTER (WHERE gm.created_ts >= :window1_end AND gm.created_ts < :window2_end) AS cnt_w2,
             abs(
                 count(*) FILTER (WHERE gm.created_ts >= :window1_start AND gm.created_ts < :window1_end) -
                 count(*) FILTER (WHERE gm.created_ts >= :window1_end AND gm.created_ts < :window2_end)
             ) AS diff
        FROM GRAPH_TABLE (snb_graph
            MATCH (t IS Tag WHERE t.tagclass_id <= 5)
            COLUMNS (t.id AS tag_id, t.name AS tag_name)
        ) gt
        LEFT JOIN GRAPH_TABLE (snb_graph
            MATCH (m IS Message)-[IS hasTag]->(t IS Tag)
            COLUMNS (t.id AS tag_id, m.created_ts AS created_ts)
        ) gm ON gm.tag_id = gt.tag_id
       GROUP BY gt.tag_id, gt.tag_name
       ORDER BY diff DESC, gt.tag_name ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi2_rows >= 1) true

-- BI-4: Top Message Creators in Country Forums via GRAPH_TABLE
WITH top_forums AS (
    SELECT g.forum_id
      FROM GRAPH_TABLE (snb_graph
          MATCH (f IS Forum)-[IS hasModerator]->(mod_p IS Person)-[IS isLocatedIn]->(city IS Place WHERE city.part_of_id = :country_id)
          COLUMNS (f.id AS forum_id)
      ) g
     ORDER BY g.forum_id ASC
     LIMIT 10
)
SELECT count(*) AS bi4_rows
  FROM (
      SELECT gm.person_id,
             gm.first_name,
             gm.last_name,
             count(gmsg.msg_id) AS message_count
        FROM top_forums tf,
             GRAPH_TABLE (snb_graph
                 MATCH (f IS Forum WHERE f.id = tf.forum_id)-[IS hasMember]->(p IS Person)
                 COLUMNS (f.id AS forum_id, p.id AS person_id, p.first_name AS first_name, p.last_name AS last_name)
             ) gm
        LEFT JOIN GRAPH_TABLE (snb_graph
            MATCH (f IS Forum)-[IS containerOf]->(m IS Message)-[IS hasCreator]->(p IS Person)
            COLUMNS (f.id AS forum_id, m.id AS msg_id, p.id AS creator_id)
        ) gmsg ON gmsg.forum_id = gm.forum_id AND gmsg.creator_id = gm.person_id
       GROUP BY gm.person_id, gm.first_name, gm.last_name
       ORDER BY message_count DESC, gm.person_id ASC
       LIMIT 20
  ) q \gset

\validate_result (:bi4_rows >= 0) true

-- BI-9: Top Thread Initiators via WITH RECURSIVE ... UNION DISTINCT ON (msg_id) + GRAPH_TABLE reply-tree expansion
WITH RECURSIVE thread_tree AS (
    SELECT g.root_post_id AS root_post_id,
           g.root_author_id AS root_author_id,
           g.root_post_id AS msg_id
      FROM GRAPH_TABLE (snb_graph
          MATCH (post IS Message WHERE post.parent_message_id IS NULL AND post.created_ts >= :window1_start AND post.created_ts <= :cutoff_ts)-[IS hasCreator]->(p IS Person)
          COLUMNS (post.id AS root_post_id, p.id AS root_author_id)
      ) g
    UNION DISTINCT ON (msg_id)
    SELECT tt.root_post_id AS root_post_id,
           tt.root_author_id AS root_author_id,
           g.reply_id AS msg_id
      FROM thread_tree tt,
           GRAPH_TABLE (snb_graph
               MATCH (reply IS Message WHERE reply.created_ts >= :window1_start AND reply.created_ts <= :cutoff_ts)-[IS replyOf]->(parent IS Message WHERE parent.id = tt.msg_id)
               COLUMNS (reply.id AS reply_id)
           ) g
)
SELECT count(*) AS bi9_rows
  FROM (
      SELECT tt.root_author_id AS person_id,
             count(DISTINCT tt.root_post_id) AS thread_count,
             count(tt.msg_id) AS message_count
        FROM thread_tree tt
       GROUP BY tt.root_author_id
       ORDER BY message_count DESC, tt.root_author_id ASC
       LIMIT 15
  ) q \gset

\validate_result (:bi9_rows >= 1) true

-- BI-11: Friend Triangles within a Country via GRAPH_TABLE
SELECT count(*) AS bi11_triangles
  FROM GRAPH_TABLE (snb_graph
      MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)-[IS knows]->(p3 IS Person)-[IS knows]->(p1 IS Person)
      WHERE p1.id < p2.id AND p2.id < p3.id
      COLUMNS (p1.city_id AS c1_id, p2.city_id AS c2_id, p3.city_id AS c3_id)
  ) tri
  JOIN GRAPH_TABLE (snb_graph
      MATCH (c1 IS Place)-[IS isPartOf]->(country IS Place)
      COLUMNS (c1.id AS city_id, country.id AS country_id)
  ) l1 ON l1.city_id = tri.c1_id
  JOIN GRAPH_TABLE (snb_graph
      MATCH (c2 IS Place)-[IS isPartOf]->(country IS Place)
      COLUMNS (c2.id AS city_id, country.id AS country_id)
  ) l2 ON l2.city_id = tri.c2_id AND l2.country_id = l1.country_id
  JOIN GRAPH_TABLE (snb_graph
      MATCH (c3 IS Place)-[IS isPartOf]->(country IS Place)
      COLUMNS (c3.id AS city_id, country.id AS country_id)
  ) l3 ON l3.city_id = tri.c3_id AND l3.country_id = l1.country_id \gset

\validate_result (:bi11_triangles >= 0) true

-- BI-19: Interaction-Weighted Shortest Path Between Two Cities via UNION DISTINCT ON (start_id, curr_id ORDER BY total_weight ASC)
WITH RECURSIVE city_paths AS (
    SELECT g.start_id AS start_id,
           g.start_id AS curr_id,
           0.0::float8 AS total_weight,
           0 AS hops
      FROM GRAPH_TABLE (snb_graph
          MATCH (p IS Person WHERE p.city_id = :city1_id)
          COLUMNS (p.id AS start_id)
      ) g
    UNION DISTINCT ON (start_id, curr_id ORDER BY total_weight ASC)
    SELECT cp.start_id AS start_id,
           g.dst_id AS curr_id,
           cp.total_weight + g.weight AS total_weight,
           cp.hops + 1 AS hops
      FROM city_paths cp,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = cp.curr_id)-[k IS knows]->(dst IS Person)
               COLUMNS (dst.id AS dst_id, k.weight AS weight)
           ) g
     WHERE cp.hops < 3
)
SELECT COALESCE(min(cp.total_weight), 0.0) AS bi19_min_weight
  FROM city_paths cp,
       GRAPH_TABLE (snb_graph
           MATCH (dst_p IS Person WHERE dst_p.id = cp.curr_id AND dst_p.city_id = :city2_id)
           COLUMNS (dst_p.id AS id)
       ) g \gset

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

-- Delete temporary reply sub-thread in micro-batch cleanup using GRAPH_TABLE step + UNION DISTINCT ON (id)
WITH RECURSIVE del_tree AS (
    SELECT :bi_reply_id::bigint AS id
    UNION DISTINCT ON (id)
    SELECT g.child_id AS id
      FROM del_tree dt,
           GRAPH_TABLE (snb_graph
               MATCH (child IS Message)-[IS replyOf]->(parent IS Message WHERE parent.id = dt.id)
               COLUMNS (child.id AS child_id)
           ) g
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
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message WHERE m.created_ts <= :cutoff_ts)
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:post_refresh_msgs > :bi1_total_msgs) true
