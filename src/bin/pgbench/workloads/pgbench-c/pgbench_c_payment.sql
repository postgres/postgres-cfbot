--
-- pgbench-c (TPC-C-inspired) Payment Transaction Script
--
-- Features:
--   - 85% home warehouse / 15% remote warehouse selection
--   - 60% customer selection by c_last(nurand(255, 0, 999)) / 40% by c_id
--   - \keying_time and \think_time pacing
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set h_amount random(100, 500000) / 100.0

\if :scale > 1 AND random(1, 100) <= 15
  \set r_w random(1, :scale - 1)
  \set c_w_id CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
  \set c_d_id random(1, 10)
\else
  \set c_w_id :w_id
  \set c_d_id :d_id
\endif

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  CALL pgbench_c_payment(:w_id, :d_id, :c_w_id, :c_d_id, NULL, :c_last, :h_amount);
\else
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_c_payment(:w_id, :d_id, :c_w_id, :c_d_id, :c_id, NULL, :h_amount);
\endif
\think_time random_exponential(1, 5, 2.0) ms
