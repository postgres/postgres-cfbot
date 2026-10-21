--
-- pgbench-graph (LDBC-Inspired Social Network Graph Benchmark Workload Suite)
-- Primary Keys, Adjacency Indexes, and Statistics
--

SET client_min_messages = warning;

-- Deduplicate any cross-worker bidirectional edges before adding primary key
DELETE FROM pgbench_graph_knows a
 USING pgbench_graph_knows b
 WHERE a.ctid < b.ctid
   AND a.src_id = b.src_id
   AND a.dst_id = b.dst_id;

ALTER TABLE pgbench_graph_persons
    ADD CONSTRAINT pgbench_graph_persons_pkey PRIMARY KEY (id);

ALTER TABLE pgbench_graph_knows
    ADD CONSTRAINT pgbench_graph_knows_pkey PRIMARY KEY (src_id, dst_id);

ALTER TABLE pgbench_graph_messages
    ADD CONSTRAINT pgbench_graph_messages_pkey PRIMARY KEY (id);

CREATE INDEX pgbench_graph_knows_dst_src_idx
    ON pgbench_graph_knows (dst_id, src_id);

CREATE INDEX pgbench_graph_messages_creator_ts_idx
    ON pgbench_graph_messages (creator_id, created_ts DESC);

ANALYZE pgbench_graph_persons;
ANALYZE pgbench_graph_knows;
ANALYZE pgbench_graph_messages;
