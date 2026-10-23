--
-- pgbench-c (TPC-C-inspired) Order-Status Transaction Script
--
-- Features:
--   - 60% customer lookup by c_last(nurand(255, 0, 999)) / 40% by c_id
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  CALL pgbench_c_order_status(:w_id, :d_id, NULL, :c_last);
\else
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_c_order_status(:w_id, :d_id, :c_id, NULL);
\endif
\think_time random_exponential(1, 5, 2.0) ms
