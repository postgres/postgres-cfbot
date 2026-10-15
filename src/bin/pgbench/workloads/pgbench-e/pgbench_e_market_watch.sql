--
-- TPC-E Market-Watch and Security-Detail Transaction Script
--

\set wl_id random(1, 100 * :scale)
\set co_min random(1, 40 * :scale)
\set co_max (:co_min + 10)
\set sec_idx random_zipfian(1, 100 * :scale, 1.2)
\set s_symb format_string('SYM%06d', :sec_idx)

CALL pgbench_e_market_watch(:wl_id, :co_min, :co_max, 0);
CALL tpce_security_detail(:s_symb);
