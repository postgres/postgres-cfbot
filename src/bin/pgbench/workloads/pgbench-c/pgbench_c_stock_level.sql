--
-- pgbench-c (TPC-C-inspired) Stock-Level Transaction Script
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set threshold random(10, 20)

CALL pgbench_c_stock_level(:w_id, :d_id, :threshold, 0);
\think_time random_exponential(1, 5, 2.0) ms
