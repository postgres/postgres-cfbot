--
-- pgbench-c (TPC-C-inspired) New-Order Transaction Script
--
-- Features:
--   - \keying_time and \think_time pacing
--   - nurand() non-uniform item and customer selection
--   - array_generate() and 0-based array subscripting
--   - 1% simulated invalid item rollback caught via \try ... \catch 'P0001'
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set c_id nurand(1023, 1, 30)
\set ol_cnt random(5, 15)
\set rbk random(1, 100)

\set i_ids array_generate(:ol_cnt, 1)
\set s_w_ids array_generate(:ol_cnt, :w_id)
\set qtys array_generate(:ol_cnt, 5)

\for idx 0 .. (:ol_cnt - 1) BY 1
  \set i_id nurand(8191, 1, 1000)
  \set i_ids[:idx] :i_id
  \set s_w_ids[:idx] :w_id
  \set qtys[:idx] random(1, 10)
  \if :scale > 1 AND random(1, 100) = 1
    \set r_w random(1, :scale - 1)
    \set s_w_ids[:idx] CASE WHEN :r_w >= :w_id THEN :r_w + 1 ELSE :r_w END
  \endif
\endfor

\if :rbk = 1
  \set i_ids[:ol_cnt - 1] -1
\endif

\try
CALL pgbench_c_new_order(:w_id, :d_id, :c_id, :ol_cnt, :i_ids, :s_w_ids, :qtys);
\catch 'P0001'
\endtry
\think_time random_exponential(1, 5, 2.0) ms
