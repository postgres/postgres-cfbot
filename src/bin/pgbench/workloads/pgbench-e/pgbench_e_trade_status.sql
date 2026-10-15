--
-- TPC-E Trade-Status, Trade-Lookup, and Trade-Update Transaction Script
--

\set ca_id random(1, 200 * :scale)
\set op random(1, 100)

\if :op <= 60
  CALL pgbench_e_trade_status(:ca_id);
\elif :op <= 90
  CALL tpce_trade_lookup(:ca_id, 10);
\else
  CALL tpce_trade_update(:ca_id, 5);
\endif
