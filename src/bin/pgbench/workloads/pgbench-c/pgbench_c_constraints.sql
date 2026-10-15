--
-- TPC-C Post-Load Secondary Indexes, Foreign Keys, and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_customer_name
    ON customer (c_w_id, c_d_id, c_last, c_first);

CREATE INDEX IF NOT EXISTS idx_orders_customer
    ON orders (o_w_id, o_d_id, o_c_id, o_id DESC);

ANALYZE warehouse, district, customer, history, new_order, orders, order_line, item, stock;
