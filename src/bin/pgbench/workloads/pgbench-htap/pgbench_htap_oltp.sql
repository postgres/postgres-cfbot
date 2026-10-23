--
-- CH-benCHmark OLTP Transactional Mix Script
--
-- Executes the full TPC-C transactional mix (45% New-Order, 43% Payment,
-- 4% Order-Status, 4% Delivery, 4% Stock-Level) for the OLTP client group.
--

\set w_id random(1, :scale)
\set d_id random(1, 10)
\set tx_type random(1, 100)

\if :tx_type <= 45
  \set c_id nurand(1023, 1, 30)
  \set ol_cnt random(5, 10)
  \set rbk random(1, 100)
  \set i_ids array_generate(:ol_cnt, 1)
  \set s_w_ids array_generate(:ol_cnt, :w_id)
  \set qtys array_generate(:ol_cnt, 5)
  \for idx 0 .. (:ol_cnt - 1) BY 1
    \set i_ids[:idx] nurand(8191, 1, 1000)
    \set s_w_ids[:idx] :w_id
    \set qtys[:idx] random(1, 10)
  \endfor
  \if :rbk = 1
    \set i_ids[:ol_cnt - 1] -1
  \endif
  \try
  CALL pgbench_c_new_order(:w_id, :d_id, :c_id, :ol_cnt, :i_ids, :s_w_ids, :qtys);
  \catch 'P0001'
  \endtry
\elif :tx_type <= 88
  \set c_id nurand(1023, 1, 30)
  \set h_amount random(100, 50000) / 100.0
  CALL pgbench_c_payment(:w_id, :d_id, :w_id, :d_id, :c_id, NULL, :h_amount);
\elif :tx_type <= 92
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_c_order_status(:w_id, :d_id, :c_id, NULL);
\elif :tx_type <= 96
  \set carrier random(1, 10)
  \for del_d 1 .. 10 BY 1
    CALL pgbench_c_delivery(:w_id, :del_d, :carrier);
  \endfor
\else
  \set threshold random(10, 20)
  CALL pgbench_c_stock_level(:w_id, :d_id, :threshold, 0);
\endif
