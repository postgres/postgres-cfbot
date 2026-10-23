--
-- pgbench-h (TPC-H-inspired) Parallel In-Database Data Generation Script for pgbench
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> -f pgbench_h_init.sql
--
-- Optional density overrides (via -D):
--   supp_per_scale   (default: 100;  full TPC-H: 10000)
--   cust_per_scale   (default: 300;  full TPC-H: 150000)
--   part_per_scale   (default: 400;  full TPC-H: 200000)
--   orders_per_scale (default: 600;  full TPC-H: 1500000)
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':supp_per_scale' FROM '^[0-9]+$')::int, 100) AS supp_per_scale,
    COALESCE(substring(':cust_per_scale' FROM '^[0-9]+$')::int, 300) AS cust_per_scale,
    COALESCE(substring(':part_per_scale' FROM '^[0-9]+$')::int, 400) AS part_per_scale,
    COALESCE(substring(':orders_per_scale' FROM '^[0-9]+$')::int, 600) AS orders_per_scale
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Region table (5 static rows, idempotent across workers)
INSERT INTO region (r_regionkey, r_name, r_comment)
VALUES
    (0, 'AFRICA',      'lar deposits. blithely final packages cajole.'),
    (1, 'AMERICA',     'hs use ironic, even requests. s'),
    (2, 'ASIA',        'ges. thinly even pinto beans ca'),
    (3, 'EUROPE',      'ly final courts cajole furiously final excuse'),
    (4, 'MIDDLE EAST', 'uickly special accounts cajole carefully blithely close')
ON CONFLICT (r_regionkey) DO NOTHING;

-- 2. Nation table (25 static rows, idempotent across workers)
INSERT INTO nation (n_nationkey, n_name, n_regionkey, n_comment)
SELECT
    n_id,
    CASE n_id
        WHEN 0 THEN 'ALGERIA'        WHEN 1 THEN 'ARGENTINA'     WHEN 2 THEN 'BRAZIL'
        WHEN 3 THEN 'CANADA'         WHEN 4 THEN 'EGYPT'         WHEN 5 THEN 'ETHIOPIA'
        WHEN 6 THEN 'FRANCE'         WHEN 7 THEN 'GERMANY'       WHEN 8 THEN 'INDIA'
        WHEN 9 THEN 'INDONESIA'      WHEN 10 THEN 'IRAN'         WHEN 11 THEN 'IRAQ'
        WHEN 12 THEN 'JAPAN'         WHEN 13 THEN 'JORDAN'       WHEN 14 THEN 'KENYA'
        WHEN 15 THEN 'MOROCCO'       WHEN 16 THEN 'MOZAMBIQUE'   WHEN 17 THEN 'PERU'
        WHEN 18 THEN 'CHINA'         WHEN 19 THEN 'ROMANIA'      WHEN 20 THEN 'SAUDI ARABIA'
        WHEN 21 THEN 'VIETNAM'       WHEN 22 THEN 'RUSSIA'       WHEN 23 THEN 'UNITED KINGDOM'
        ELSE 'UNITED STATES'
    END,
    CASE n_id
        WHEN 0 THEN 0 WHEN 1 THEN 1 WHEN 2 THEN 1 WHEN 3 THEN 1 WHEN 4 THEN 4
        WHEN 5 THEN 0 WHEN 6 THEN 3 WHEN 7 THEN 3 WHEN 8 THEN 2 WHEN 9 THEN 2
        WHEN 10 THEN 4 WHEN 11 THEN 4 WHEN 12 THEN 2 WHEN 13 THEN 4 WHEN 14 THEN 0
        WHEN 15 THEN 0 WHEN 16 THEN 0 WHEN 17 THEN 1 WHEN 18 THEN 2 WHEN 19 THEN 3
        WHEN 20 THEN 4 WHEN 21 THEN 2 WHEN 22 THEN 3 WHEN 23 THEN 3 ELSE 1
    END,
    pgbench.random_astring(20, 60)
FROM generate_series(0, 24) AS g(n_id)
ON CONFLICT (n_nationkey) DO NOTHING;

-- 3. Supplier table (partitioned across workers by s_suppkey)
INSERT INTO supplier (s_suppkey, s_name, s_address, s_nationkey, s_phone, s_acctbal, s_comment)
SELECT
    s_id,
    pgbench.format_string('Supplier#%09d', s_id)::char(25),
    pgbench.random_astring(15, 35),
    (s_id % 25)::int,
    pgbench.format_string('%02d-%03d-%03d-%04d', 10 + (s_id % 25), (s_id * 3) % 900 + 100, (s_id * 7) % 900 + 100, (s_id * 13) % 9000 + 1000)::char(15),
    (pgbench.random(-99999, 999999) / 100.0)::numeric(15,2),
    pgbench.random_astring(25, 60)
FROM generate_series(:client_id + 1, :supp_per_scale * :scale, :num_workers) AS g(s_id);

-- 4. Customer table (partitioned across workers by c_custkey)
INSERT INTO customer (c_custkey, c_name, c_address, c_nationkey, c_phone, c_acctbal, c_mktsegment, c_comment)
SELECT
    c_id,
    pgbench.format_string('Customer#%09d', c_id),
    pgbench.random_astring(15, 35),
    (c_id % 25)::int,
    pgbench.format_string('%02d-%03d-%03d-%04d', 10 + (c_id % 25), (c_id * 5) % 900 + 100, (c_id * 11) % 900 + 100, (c_id * 17) % 9000 + 1000)::char(15),
    (pgbench.random(-99999, 999999) / 100.0)::numeric(15,2),
    CASE c_id % 5
        WHEN 0 THEN 'AUTOMOBILE'
        WHEN 1 THEN 'BUILDING'
        WHEN 2 THEN 'FURNITURE'
        WHEN 3 THEN 'MACHINERY'
        ELSE 'HOUSEHOLD'
    END::char(10),
    pgbench.random_astring(30, 70)
FROM generate_series(:client_id + 1, :cust_per_scale * :scale, :num_workers) AS g(c_id);

-- 5. Part table (partitioned across workers by p_partkey)
INSERT INTO part (p_partkey, p_name, p_mfgr, p_brand, p_type, p_size, p_container, p_retailprice, p_comment)
SELECT
    p_id,
    CASE WHEN p_id % 5 = 0 THEN 'PROMO ' ELSE 'STANDARD ' END || pgbench.random_astring(10, 25),
    pgbench.format_string('Manufacturer#%d', (p_id % 5) + 1)::char(25),
    pgbench.format_string('Brand#%d%d', ((p_id - 1) % 5) + 1, ((p_id / 5) % 5) + 1)::char(10),
    CASE p_id % 4
        WHEN 0 THEN 'PROMO BURNISHED COPPER'
        WHEN 1 THEN 'STANDARD ANODIZED TIN'
        WHEN 2 THEN 'ECONOMY POLISHED BRASS'
        ELSE 'LARGE PLATED STEEL'
    END,
    ((p_id - 1) % 50) + 1,
    CASE p_id % 12
        WHEN 0 THEN 'SM CASE'   WHEN 1 THEN 'SM BOX'    WHEN 2 THEN 'SM PACK'   WHEN 3 THEN 'SM PKG'
        WHEN 4 THEN 'MED BAG'   WHEN 5 THEN 'MED BOX'   WHEN 6 THEN 'MED PKG'   WHEN 7 THEN 'MED PACK'
        WHEN 8 THEN 'LG CASE'   WHEN 9 THEN 'LG BOX'    WHEN 10 THEN 'LG PACK'  ELSE 'LG PKG'
    END::char(10),
    ((90000 + ((p_id / 10) % 20001) + 100 * (p_id % 1000)) / 100.0)::numeric(15,2),
    pgbench.random_astring(8, 20)
FROM generate_series(:client_id + 1, :part_per_scale * :scale, :num_workers) AS g(p_id);

-- 6. PartSupp table (4 suppliers per part, partitioned by ps_partkey)
INSERT INTO partsupp (ps_partkey, ps_suppkey, ps_availqty, ps_supplycost, ps_comment)
SELECT
    p_id,
    ((p_id + s_off * ((:supp_per_scale * :scale) / 4 + 1) - 1) % (:supp_per_scale * :scale)) + 1,
    pgbench.random(1, 9999)::int,
    (pgbench.random(100, 100000) / 100.0)::numeric(15,2),
    pgbench.random_astring(40, 100)
FROM generate_series(:client_id + 1, :part_per_scale * :scale, :num_workers) AS g(p_id)
CROSS JOIN generate_series(0, 3) AS s(s_off);

-- 7. Orders table (partitioned across workers by o_orderkey)
INSERT INTO orders (
    o_orderkey, o_custkey, o_orderstatus, o_totalprice,
    o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment
)
SELECT
    o_id,
    ((o_id * 7 - 1) % (:cust_per_scale * :scale)) + 1,
    CASE WHEN o_id % 3 = 0 THEN 'F' WHEN o_id % 3 = 1 THEN 'O' ELSE 'P' END,
    (pgbench.random(100000, 25000000) / 100.0)::numeric(15,2),
    pgbench.int_to_date('1992-01-01', o_id, :orders_per_scale * :scale, '2400 days')::date,
    CASE o_id % 5
        WHEN 0 THEN '1-URGENT'
        WHEN 1 THEN '2-HIGH'
        WHEN 2 THEN '3-MEDIUM'
        WHEN 3 THEN '4-NOT SPECIFIED'
        ELSE '5-LOW'
    END::char(15),
    pgbench.format_string('Clerk#%09d', (o_id % 1000) + 1)::char(15),
    0,
    pgbench.random_astring(20, 60)
FROM generate_series(:client_id + 1, :orders_per_scale * :scale, :num_workers) AS g(o_id);

-- 8. Lineitem table (partitioned across workers by l_orderkey)
INSERT INTO lineitem (
    l_orderkey, l_partkey, l_suppkey, l_linenumber,
    l_quantity, l_extendedprice, l_discount, l_tax,
    l_returnflag, l_linestatus,
    l_shipdate, l_commitdate, l_receiptdate,
    l_shipinstruct, l_shipmode, l_comment
)
SELECT
    o_id,
    ((o_id + l_num - 1) % (:part_per_scale * :scale)) + 1,
    (((o_id + l_num - 1) % (:part_per_scale * :scale)) + ((l_num - 1) * ((:supp_per_scale * :scale) / 4 + 1))) % (:supp_per_scale * :scale) + 1,
    l_num,
    ((l_num * 7 + o_id) % 50 + 1)::numeric(15,2),
    (((l_num * 7 + o_id) % 50 + 1) * 125.50)::numeric(15,2),
    ((o_id + l_num) % 11 / 100.0)::numeric(15,2),
    ((o_id + l_num) % 9 / 100.0)::numeric(15,2),
    CASE WHEN (o_id + l_num) % 3 = 0 THEN 'R' WHEN (o_id + l_num) % 3 = 1 THEN 'A' ELSE 'N' END,
    CASE WHEN o_id % 2 = 0 THEN 'F' ELSE 'O' END,
    pgbench.date_add(pgbench.int_to_date('1992-01-01', o_id, :orders_per_scale * :scale, '2400 days'), (l_num * 11) % 60 + 1, 'day')::date,
    pgbench.date_add(pgbench.int_to_date('1992-01-01', o_id, :orders_per_scale * :scale, '2400 days'), (l_num * 7) % 45 + 15, 'day')::date,
    pgbench.date_add(pgbench.int_to_date('1992-01-01', o_id, :orders_per_scale * :scale, '2400 days'), (l_num * 11) % 60 + 10, 'day')::date,
    CASE l_num % 4
        WHEN 0 THEN 'DELIVER IN PERSON'
        WHEN 1 THEN 'COLLECT COD'
        WHEN 2 THEN 'NONE'
        ELSE 'TAKE BACK RETURN'
    END::char(25),
    CASE (o_id + l_num) % 7
        WHEN 0 THEN 'REG AIR'
        WHEN 1 THEN 'AIR'
        WHEN 2 THEN 'RAIL'
        WHEN 3 THEN 'SHIP'
        WHEN 4 THEN 'TRUCK'
        WHEN 5 THEN 'MAIL'
        ELSE 'FOB'
    END::char(10),
    pgbench.random_astring(12, 35)
FROM generate_series(:client_id + 1, :orders_per_scale * :scale, :num_workers) AS g(o_id)
CROSS JOIN LATERAL generate_series(1, 1 + (o_id % 4)) AS l(l_num);
