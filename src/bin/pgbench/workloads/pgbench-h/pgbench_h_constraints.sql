--
-- TPC-H Post-Load Secondary Indexes and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_lineitem_shipdate
    ON lineitem (l_shipdate);

CREATE INDEX IF NOT EXISTS idx_lineitem_part_supp
    ON lineitem (l_partkey, l_suppkey);

CREATE INDEX IF NOT EXISTS idx_orders_custkey_orderdate
    ON orders (o_custkey, o_orderdate);

CREATE INDEX IF NOT EXISTS idx_customer_nationkey_mktsegment
    ON customer (c_nationkey, c_mktsegment);

CREATE INDEX IF NOT EXISTS idx_supplier_nationkey
    ON supplier (s_nationkey);

ANALYZE region, nation, supplier, customer, part, partsupp, orders, lineitem;
