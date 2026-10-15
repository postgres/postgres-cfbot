--
-- pgbench-c (TPC-C-inspired) Parallel In-Database Data Generation Script for pgbench
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> -f pgbench_c_init.sql
--
-- Optional density overrides (via -D):
--   items_per_scale (default: 1000; full TPC-C specification: 100000)
--   cust_per_dist   (default: 30;   full TPC-C specification: 3000)
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':items_per_scale' FROM '^[0-9]+$')::int, 1000) AS items_per_scale,
    COALESCE(substring(':cust_per_dist' FROM '^[0-9]+$')::int, 30) AS cust_per_dist
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Item table (partitioned across workers by i_id)
INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data)
SELECT
    i_id,
    pgbench.random(1, 10000)::int,
    pgbench.format_string('ITEM-%06d-%s', i_id, pgbench.random_astring(4, 10)),
    (pgbench.random(100, 10000) / 100.0)::numeric(5,2),
    CASE WHEN pgbench.random(1, 10) = 1
         THEN pgbench.random_astring(10, 20) || 'ORIGINAL' || pgbench.random_astring(8, 12)
         ELSE pgbench.random_astring(26, 50)
    END
FROM generate_series(:client_id + 1, :items_per_scale, :num_workers) AS g(i_id);

-- 2. Warehouse table (partitioned across workers by w_id)
INSERT INTO warehouse (w_id, w_name, w_street_1, w_street_2, w_city, w_state, w_zip, w_tax, w_ytd)
SELECT
    w_id,
    pgbench.format_string('WH-%04d', w_id),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    chr(65 + (w_id % 25)) || chr(65 + ((w_id / 25) % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    (pgbench.random(0, 2000) / 10000.0)::numeric(4,4),
    300000.00
FROM generate_series(:client_id + 1, :scale, :num_workers) AS g(w_id);

-- 3. Stock table (partitioned across workers by s_i_id)
INSERT INTO stock (
    s_i_id, s_w_id, s_quantity,
    s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05,
    s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10,
    s_ytd, s_order_cnt, s_remote_cnt, s_data
)
SELECT
    i_id,
    w_id,
    pgbench.random(10, 100)::smallint,
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    pgbench.random_astring(24, 24),
    0, 0, 0,
    CASE WHEN pgbench.random(1, 10) = 1
         THEN pgbench.random_astring(10, 20) || 'ORIGINAL' || pgbench.random_astring(8, 12)
         ELSE pgbench.random_astring(26, 50)
    END
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(:client_id + 1, :items_per_scale, :num_workers) AS i(i_id);

-- 4. District table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO district (
    d_id, d_w_id, d_name, d_street_1, d_street_2, d_city, d_state, d_zip,
    d_tax, d_ytd, d_next_o_id
)
SELECT
    d_id::smallint,
    w_id,
    pgbench.format_string('DIST-%02d-%02d', w_id, d_id),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    chr(65 + ((w_id + d_id) % 25)) || chr(65 + (d_id % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    (pgbench.random(0, 2000) / 10000.0)::numeric(4,4),
    30000.00,
    :cust_per_dist + 1
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 5. Customer table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO customer (
    c_id, c_d_id, c_w_id, c_first, c_middle, c_last,
    c_street_1, c_street_2, c_city, c_state, c_zip, c_phone,
    c_since, c_credit, c_credit_lim, c_discount,
    c_balance, c_ytd_payment, c_payment_cnt, c_delivery_cnt, c_data
)
SELECT
    c_id,
    d_id::smallint,
    w_id,
    pgbench.random_astring(8, 16),
    'OE',
    CASE WHEN c_id <= 1000
         THEN pgbench.c_last((c_id - 1) % 1000)
         ELSE pgbench.c_last(pgbench.nurand(255, 0, 999))
    END,
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    pgbench.random_astring(10, 20),
    chr(65 + ((c_id + d_id + w_id) % 25)) || chr(65 + (c_id % 25)),
    pgbench.random_nstring(4, 4) || '11111',
    pgbench.random_nstring(16, 16),
    pgbench.date_add('2020-01-01', c_id % 365, 'day')::timestamp,
    CASE WHEN pgbench.random(1, 10) = 1 THEN 'BC' ELSE 'GC' END,
    50000.00,
    (pgbench.random(0, 5000) / 10000.0)::numeric(4,4),
    -10.00,
    10.00,
    1,
    0,
    pgbench.random_astring(30, 60)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS c(c_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 6. History table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
SELECT
    c_id,
    d_id::smallint,
    w_id,
    d_id::smallint,
    w_id,
    pgbench.date_add('2020-01-01', c_id % 365, 'day')::timestamp,
    10.00,
    pgbench.random_astring(12, 24)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS c(c_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 7. Orders table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
SELECT
    o_id,
    d_id::smallint,
    w_id,
    (pgbench.permute(o_id - 1, :cust_per_dist, w_id * 100 + d_id) + 1)::int,
    pgbench.date_add('2020-01-01', o_id % 365, 'day')::timestamp,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN pgbench.random(1, 10)::smallint
         ELSE NULL
    END,
    (5 + ((o_id + d_id + w_id) % 6))::smallint,
    1
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS o(o_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 8. New-Order table (last 30% of orders per district)
INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
SELECT
    o_id,
    d_id::smallint,
    w_id
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(((:cust_per_dist * 7) / 10) + 1, :cust_per_dist) AS o(o_id)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;

-- 9. Order-Line table (partitioned across workers by (w_id, d_id) slice)
INSERT INTO order_line (
    ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
    ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info
)
SELECT
    o_id,
    d_id::smallint,
    w_id,
    ol_num::smallint,
    pgbench.random(1, :items_per_scale)::int,
    w_id,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN pgbench.date_add('2020-01-02', o_id % 365, 'day')::timestamp
         ELSE NULL
    END,
    5,
    CASE WHEN o_id <= (:cust_per_dist * 7) / 10
         THEN 0.00
         ELSE (pgbench.random(10, 5000) / 100.0)::numeric(6,2)
    END,
    pgbench.random_astring(24, 24)
FROM generate_series(1, :scale) AS w(w_id)
CROSS JOIN generate_series(1, 10) AS d(d_id)
CROSS JOIN generate_series(1, :cust_per_dist) AS o(o_id)
CROSS JOIN LATERAL generate_series(1, 5 + ((o_id + d_id + w_id) % 6)) AS l(ol_num)
WHERE ((w_id - 1) * 10 + (d_id - 1)) % :num_workers = :client_id;
