--
-- CH-benCHmark Post-Load Secondary Indexes and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_ch_customer_name
    ON customer (c_w_id, c_d_id, c_last, c_first);

CREATE INDEX IF NOT EXISTS idx_ch_orders_customer
    ON orders (o_w_id, o_d_id, o_c_id, o_id DESC);

CREATE INDEX IF NOT EXISTS idx_htap_supplier_nation
    ON htap_supplier (su_nationkey);

ANALYZE warehouse, district, customer, history, new_order, orders, order_line,
        item, stock, htap_region, htap_nation, htap_supplier;
