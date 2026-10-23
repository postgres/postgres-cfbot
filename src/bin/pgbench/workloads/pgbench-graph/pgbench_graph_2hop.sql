--
-- pgbench-graph: 2-Hop Friend-of-Friend Discovery & Scoring (LDBC IC-1 / IC-9 style)
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

WITH hop1 AS (
    SELECT dst_id
      FROM pgbench_graph_knows
     WHERE src_id = :person_id
),
hop2 AS (
    SELECT k2.dst_id AS fof_id,
           count(*) AS mutual_friends,
           sum(k2.weight) AS total_weight
      FROM hop1 h1
      JOIN pgbench_graph_knows k2 ON k2.src_id = h1.dst_id
     WHERE k2.dst_id <> :person_id
       AND NOT EXISTS (
           SELECT 1
             FROM hop1 d
            WHERE d.dst_id = k2.dst_id
       )
     GROUP BY k2.dst_id
)
SELECT p.id,
       p.first_name,
       p.last_name,
       p.city_id,
       h2.mutual_friends,
       h2.total_weight
  FROM hop2 h2
  JOIN pgbench_graph_persons p ON p.id = h2.fof_id
 ORDER BY h2.mutual_friends DESC, h2.total_weight DESC, p.id ASC
 LIMIT 10;
