--
-- pgbench-h (TPC-H-inspired) Refresh Functions (RF1 New Sales Insert & RF2 Old Sales Delete)
--
-- Uses \for loop to insert a batch of new orders + lineitems (RF1) and
-- purge the refreshed batch (RF2).
--

\set batch_size 5
\set base_okey (9000000 + :client_id * 10000 + random(1, 5000))

-- RF1: Insert batch of new orders and lineitems
BEGIN;
\for i 1 .. :batch_size BY 1
  \set okey (:base_okey + :i)
  \set ckey random(1, 300 * :scale)
  \set pkey random(1, 400 * :scale)
  \set skey random(1, 100 * :scale)
  \set odate int_to_date('1995-01-01', random(0, 1000), 'day')
  \set sdate date_add(:odate, random(1, 30), 'day')
  \set cdate date_add(:odate, 15, 'day')
  \set rdate date_add(:sdate, 5, 'day')

  INSERT INTO orders (
      o_orderkey, o_custkey, o_orderstatus, o_totalprice,
      o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment
  ) VALUES (
      :okey, :ckey, 'O', 1250.00,
      :odate::date, '3-MEDIUM', pgbench.format_string('Clerk#%09d', :i), 0, 'RF1 refresh order'
  ) ON CONFLICT (o_orderkey) DO NOTHING;

  INSERT INTO lineitem (
      l_orderkey, l_partkey, l_suppkey, l_linenumber,
      l_quantity, l_extendedprice, l_discount, l_tax,
      l_returnflag, l_linestatus,
      l_shipdate, l_commitdate, l_receiptdate,
      l_shipinstruct, l_shipmode, l_comment
  ) VALUES (
      :okey, :pkey, :skey, 1,
      10.00, 1250.00, 0.05, 0.02,
      'N', 'O',
      :sdate::date, :cdate::date, :rdate::date,
      'DELIVER IN PERSON', 'AIR', 'RF1 refresh lineitem'
  ) ON CONFLICT (l_orderkey, l_linenumber) DO NOTHING;
\endfor
COMMIT;

-- RF2: Delete batch of expired/refreshed orders and lineitems
BEGIN;
\for i 1 .. :batch_size BY 1
  \set okey (:base_okey + :i)
  DELETE FROM lineitem WHERE l_orderkey = :okey;
  DELETE FROM orders WHERE o_orderkey = :okey;
\endfor
COMMIT;
