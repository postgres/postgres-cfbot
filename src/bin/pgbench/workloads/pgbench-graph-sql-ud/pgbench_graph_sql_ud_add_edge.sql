--
-- pgbench-graph-sql-ud: Transactional Edge & Message Insertion with Reservoir Push & Watermark
--

\set src_id random_zipfian(1, 200 * :scale, 1.2)
\set dst_id random(1, 200 * :scale)

CALL pgbench_graph_add_edge(:src_id, :dst_id, 1.0, 'graph-stream-post', NULL) \gset edge_

\if reservoir_count('persons') >= 0
\reservoir_push persons :src_id
\reservoir_push persons :dst_id
\endif
\advance_watermark :edge_p_msg_id
