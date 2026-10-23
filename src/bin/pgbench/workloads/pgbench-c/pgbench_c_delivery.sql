--
-- pgbench-c (TPC-C-inspired) Delivery Transaction Script
--
-- Features:
--   - Iterates across all 10 districts in the warehouse using \for
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set o_carrier_id random(1, 10)

\for d_id 1 .. 10 BY 1
  CALL pgbench_c_delivery(:w_id, :d_id, :o_carrier_id);
\endfor
\think_time random_exponential(1, 5, 2.0) ms
