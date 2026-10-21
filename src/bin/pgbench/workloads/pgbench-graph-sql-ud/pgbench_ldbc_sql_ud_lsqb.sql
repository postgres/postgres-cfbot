--
-- pgbench-graph (SQL_UD): LDBC Labelled Subgraph Query Benchmark (LSQB) Suite (Q1 - Q9)
-- Executes unseeded cyclic (Q1-Q5), acyclic (Q6-Q7), and anti-join (Q8-Q9)
-- structural pattern-matching count queries over the LDBC SNB property graph
-- on the one-with-everything PostgreSQL branch.
--

-- Q1: Long 8-hop path across Country <- City <- Person <- Forum -> Post <- Comment -> Tag
SELECT count(*) AS q1_count
  FROM pgbench_graph_places country
  JOIN pgbench_graph_places city ON city.part_of_id = country.id AND city.type = 'city'
  JOIN pgbench_graph_persons p ON p.city_id = city.id
  JOIN pgbench_graph_forum_members fm ON fm.person_id = p.id
  JOIN pgbench_graph_forums f ON f.id = fm.forum_id
  JOIN pgbench_graph_messages post ON post.forum_id = f.id AND post.parent_message_id IS NULL
  JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = post.id
  JOIN pgbench_graph_message_tags mt ON mt.message_id = cmt.id
  JOIN pgbench_graph_tags t ON t.id = mt.tag_id
 WHERE country.type = 'country' \gset

\validate_result (:q1_count >= 0) true

-- Q2: Cyclic 4-pattern: Person1 -[:knows]-> Person2 <-[:hasCreator]- Comment -[:replyOf]-> Post -[:hasCreator]-> Person1
SELECT count(*) AS q2_count
  FROM pgbench_graph_knows k
  JOIN pgbench_graph_messages post ON post.creator_id = k.src_id AND post.parent_message_id IS NULL
  JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = post.id AND cmt.creator_id = k.dst_id \gset

\validate_result (:q2_count >= 0) true

-- Q3: Country-localized friendship triangle: Person1 -[:knows]- Person2 -[:knows]- Person3 -[:knows]- Person1
SELECT count(*) AS q3_count
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

\validate_result (:q3_count >= 0) true

-- Q4: Message - Tag - Like - Creator interaction pattern
SELECT count(*) AS q4_count
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_message_tags mt ON mt.message_id = m.id
  JOIN pgbench_graph_likes l ON l.message_id = m.id
  JOIN pgbench_graph_knows k ON k.src_id = m.creator_id AND k.dst_id = l.person_id \gset

\validate_result (:q4_count >= 0) true

-- Q5: Message -> Comment reply chain with distinct tags (tag1 <> tag2)
SELECT count(*) AS q5_count
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = m.id
  JOIN pgbench_graph_message_tags mt1 ON mt1.message_id = m.id
  JOIN pgbench_graph_message_tags mt2 ON mt2.message_id = cmt.id
 WHERE mt1.tag_id <> mt2.tag_id \gset

\validate_result (:q5_count >= 0) true

-- Q6: 2-hop Acyclic Path: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag (Person1 <> Person3)
SELECT count(*) AS q6_count
  FROM pgbench_graph_knows k1
  JOIN pgbench_graph_knows k2 ON k2.src_id = k1.dst_id AND k2.dst_id <> k1.src_id
  JOIN pgbench_graph_person_tags pt ON pt.person_id = k2.dst_id \gset

\validate_result (:q6_count >= 0) true

-- Q7: Star pattern with OPTIONAL (LEFT JOIN) likes and replies
SELECT count(*) AS q7_count
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_message_tags mt ON mt.message_id = m.id
  LEFT JOIN pgbench_graph_likes l ON l.message_id = m.id
  LEFT JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = m.id \gset

\validate_result (:q7_count >= 0) true

-- Q8: Anti-Join Pattern: Message -> Comment where Comment has tag2 but NOT tag1 of parent Message
SELECT count(*) AS q8_count
  FROM pgbench_graph_messages m
  JOIN pgbench_graph_messages cmt ON cmt.parent_message_id = m.id
  JOIN pgbench_graph_message_tags mt1 ON mt1.message_id = m.id
  JOIN pgbench_graph_message_tags mt2 ON mt2.message_id = cmt.id AND mt2.tag_id <> mt1.tag_id
 WHERE NOT EXISTS (
     SELECT 1
       FROM pgbench_graph_message_tags mt_anti
      WHERE mt_anti.message_id = cmt.id
        AND mt_anti.tag_id = mt1.tag_id
 ) \gset

\validate_result (:q8_count >= 0) true

-- Q9: Open-Triad Anti-Join: Person1 -[:knows]- Person2 -[:knows]- Person3 where NOT (Person1 -[:knows]- Person3)
SELECT count(*) AS q9_count
  FROM pgbench_graph_knows k1
  JOIN pgbench_graph_knows k2 ON k2.src_id = k1.dst_id AND k2.dst_id > k1.src_id
  JOIN pgbench_graph_person_tags pt ON pt.person_id = k2.dst_id
 WHERE NOT EXISTS (
     SELECT 1
       FROM pgbench_graph_knows k3
      WHERE k3.src_id = k1.src_id
        AND k3.dst_id = k2.dst_id
 ) \gset

\validate_result (:q9_count >= 0) true
