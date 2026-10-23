--
-- pgbench-graph-pgq: 1-Hop Neighborhood & Recent Messages Query (LDBC IS-3 / IC-2 style)
-- Uses ISO SQL/PGQ GRAPH_TABLE pattern matching over snb_graph.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

SELECT g.friend_id,
       g.first_name,
       g.last_name,
       g.weight,
       m.msg_id,
       m.created_ts
  FROM GRAPH_TABLE (snb_graph
      MATCH (src IS Person WHERE src.id = :person_id)-[k IS knows]->(friend IS Person)
      COLUMNS (
          friend.id AS friend_id,
          friend.first_name AS first_name,
          friend.last_name AS last_name,
          k.weight AS weight
      )
  ) g
  LEFT JOIN LATERAL (
      SELECT gm.msg_id, gm.created_ts
        FROM GRAPH_TABLE (snb_graph
            MATCH (msg IS Message)-[IS hasCreator]->(author IS Person WHERE author.id = g.friend_id)
            COLUMNS (
                msg.id AS msg_id,
                msg.created_ts AS created_ts
            )
        ) gm
       ORDER BY gm.created_ts DESC
       LIMIT 2
  ) m ON true
 ORDER BY g.weight DESC, g.friend_id ASC
 LIMIT 10;
