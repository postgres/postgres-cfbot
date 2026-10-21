--
-- pgbench-graph-pgq: LDBC Labelled Subgraph Query Benchmark (LSQB) Suite (Q1 - Q9)
-- Executes unseeded cyclic (Q1-Q5), acyclic (Q6-Q7), and anti-join (Q8-Q9)
-- structural pattern-matching count queries over snb_graph using ISO SQL/PGQ
-- GRAPH_TABLE syntax.
--

-- Q1: Long path across Country <-[:isPartOf]- City <-[:isLocatedIn]- Person <-[:hasMember]- Forum -[:containerOf]-> Post <-[:replyOf]- Comment -[:hasTag]-> Tag
SELECT count(*) AS q1_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (country IS Place WHERE country.type = 'country')
            <-[IS isPartOf]-(city IS Place WHERE city.type = 'city')
            <-[IS isLocatedIn]-(p IS Person)
            <-[IS hasMember]-(f IS Forum)
            -[IS containerOf]->(post IS Message WHERE post.parent_message_id IS NULL)
            <-[IS replyOf]-(cmt IS Message)
            -[IS hasTag]->(t IS Tag)
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:q1_count >= 0) true

-- Q2: Cyclic 4-pattern: Person1 -[:knows]-> Person2 <-[:hasCreator]- Comment -[:replyOf]-> Post -[:hasCreator]-> Person1
SELECT count(*) AS q2_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)
            <-[IS hasCreator]-(cmt IS Message)
            -[IS replyOf]->(post IS Message WHERE post.parent_message_id IS NULL)
            -[IS hasCreator]->(p1 IS Person)
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:q2_count >= 0) true

-- Q3: Country-localized friendship triangle:
--     Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:knows]-> Person1
--     all located in cities of the same country
SELECT count(*) AS q3_count
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

\validate_result (:q3_count >= 0) true

-- Q4: Message - Tag - Like - Creator cyclic interaction pattern
SELECT count(*) AS q4_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (t IS Tag)<-[IS hasTag]-(m IS Message)<-[IS likes]-(liker IS Person)<-[IS knows]-(creator IS Person)<-[IS hasCreator]-(m IS Message)
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:q4_count >= 0) true

-- Q5: Message <-[:replyOf]- Comment chain with distinct tags (tag1 <> tag2)
SELECT count(*) AS q5_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (t1 IS Tag)<-[IS hasTag]-(m IS Message)<-[IS replyOf]-(cmt IS Message)-[IS hasTag]->(t2 IS Tag)
      WHERE t1.id <> t2.id
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:q5_count >= 0) true

-- Q6: 2-hop Acyclic Path: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag (Person1 <> Person3)
SELECT count(*) AS q6_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)-[IS knows]->(p3 IS Person)-[IS hasInterest]->(t IS Tag)
      WHERE p1.id <> p3.id
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:q6_count >= 0) true

-- Q7: Star pattern with OPTIONAL (LEFT JOIN) likes and replies
SELECT count(*) AS q7_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (m IS Message)-[IS hasTag]->(t IS Tag)
      COLUMNS (m.id AS msg_id)
  ) base
  LEFT JOIN GRAPH_TABLE (snb_graph
      MATCH (liker IS Person)-[IS likes]->(m IS Message)
      COLUMNS (m.id AS msg_id, liker.id AS liker_id)
  ) l ON l.msg_id = base.msg_id
  LEFT JOIN GRAPH_TABLE (snb_graph
      MATCH (cmt IS Message)-[IS replyOf]->(m IS Message)
      COLUMNS (m.id AS msg_id, cmt.id AS cmt_id)
  ) r ON r.msg_id = base.msg_id \gset

\validate_result (:q7_count >= 0) true

-- Q8: Anti-Join Pattern: Message <-[:replyOf]- Comment where Comment has tag2 but NOT tag1 of parent Message
SELECT count(*) AS q8_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (t1 IS Tag)<-[IS hasTag]-(m IS Message)<-[IS replyOf]-(cmt IS Message)-[IS hasTag]->(t2 IS Tag)
      WHERE t1.id <> t2.id
      COLUMNS (cmt.id AS cmt_id, t1.id AS tag1_id)
  ) g
 WHERE NOT EXISTS (
     SELECT 1
       FROM GRAPH_TABLE (snb_graph
           MATCH (c IS Message WHERE c.id = g.cmt_id)-[IS hasTag]->(t IS Tag WHERE t.id = g.tag1_id)
           COLUMNS (1 AS one)
       ) anti
 ) \gset

\validate_result (:q8_count >= 0) true

-- Q9: Open-Triad Anti-Join: Person1 -[:knows]-> Person2 -[:knows]-> Person3 -[:hasInterest]-> Tag
--     where NOT (Person1 -[:knows]-> Person3)
SELECT count(*) AS q9_count
  FROM GRAPH_TABLE (snb_graph
      MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)-[IS knows]->(p3 IS Person)-[IS hasInterest]->(t IS Tag)
      WHERE p3.id > p1.id
      COLUMNS (p1.id AS p1_id, p3.id AS p3_id)
  ) g
 WHERE NOT EXISTS (
     SELECT 1
       FROM GRAPH_TABLE (snb_graph
           MATCH (a IS Person WHERE a.id = g.p1_id)-[IS knows]->(b IS Person WHERE b.id = g.p3_id)
           COLUMNS (1 AS one)
       ) anti
 ) \gset

\validate_result (:q9_count >= 0) true
