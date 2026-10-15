--
-- TPC-E Trade-Result and Market-Feed Transaction Script
--

\set num_trades (300 * :scale)
\set num_securities (100 * :scale)
\set t_id random(1, :num_trades)
\set trade_price random_gaussian(2000, 8000, 2.5) / 100.0
\set sec_idx random_zipfian(1, :num_securities, 1.2)
\set s_symb format_string('SYM%06d', :sec_idx)
\set feed_qty random(1, 10) * 100

CALL pgbench_e_trade_result(:t_id, :trade_price);
CALL tpce_market_feed(:s_symb, :trade_price, :feed_qty);
