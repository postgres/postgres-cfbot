--
-- pgbench-graph-pgq: 2-Hop Friend-of-Friend Discovery & Scoring (LDBC IC-1 / IC-9 style)
-- Uses ISO SQL/PGQ GRAPH_TABLE 2-hop path matching over snb_graph.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

SELECT g.fof_id AS id,
       g.first_name,
       g.last_name,
       g.city_id,
       count(*) AS mutual_friends,
       sum(g.w2) AS total_weight
  FROM GRAPH_TABLE (snb_graph
      MATCH (p1 IS Person WHERE p1.id = :person_id)-[k1 IS knows]->(p2 IS Person)-[k2 IS knows]->(p3 IS Person)
      WHERE p3.id <> p1.id
      COLUMNS (
          p3.id AS fof_id,
          p3.first_name AS first_name,
          p3.last_name AS last_name,
          p3.city_id AS city_id,
          k2.weight AS w2
      )
  ) g
 WHERE NOT EXISTS (
     SELECT 1
       FROM GRAPH_TABLE (snb_graph
           MATCH (src IS Person WHERE src.id = :person_id)-[IS knows]->(direct IS Person WHERE direct.id = g.fof_id)
           COLUMNS (1 AS one)
       ) d
 )
 GROUP BY g.fof_id, g.first_name, g.last_name, g.city_id
 ORDER BY mutual_friends DESC, total_weight DESC, g.fof_id ASC
 LIMIT 10;
