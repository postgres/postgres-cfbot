--
-- pgbench-ds (TPC-DS-inspired) Parallel In-Database Data Generation Script for pgbench
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> -f pgbench_ds_init.sql
--
-- Optional density overrides (via -D):
--   days_count      (default: 730;  2 years 2000-01-01..2001-12-30)
--   items_per_scale (default: 200;  full TPC-DS: 18000)
--   cust_per_scale  (default: 200;  full TPC-DS: 100000)
--   sales_per_scale (default: 600;  full TPC-DS: 288000)
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':days_count' FROM '^[0-9]+$')::int, 730) AS days_count,
    COALESCE(substring(':items_per_scale' FROM '^[0-9]+$')::int, 200) AS items_per_scale,
    COALESCE(substring(':cust_per_scale' FROM '^[0-9]+$')::int, 200) AS cust_per_scale,
    COALESCE(substring(':sales_per_scale' FROM '^[0-9]+$')::int, 600) AS sales_per_scale
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Date-Dim table (partitioned across workers by d_date_sk)
INSERT INTO date_dim (
    d_date_sk, d_date_id, d_date, d_month_seq, d_week_seq, d_quarter_seq,
    d_year, d_dow, d_moy, d_dom, d_qoy, d_fy_year, d_weekend
)
SELECT
    d_sk,
    pgbench.format_string('AAAAAAAA%08X', d_sk)::char(16),
    dt,
    (EXTRACT(YEAR FROM dt)::int - 2000) * 12 + EXTRACT(MONTH FROM dt)::int,
    ((d_sk - 1) / 7) + 1,
    (EXTRACT(YEAR FROM dt)::int - 2000) * 4 + EXTRACT(QUARTER FROM dt)::int,
    EXTRACT(YEAR FROM dt)::int,
    EXTRACT(DOW FROM dt)::int,
    EXTRACT(MONTH FROM dt)::int,
    EXTRACT(DAY FROM dt)::int,
    EXTRACT(QUARTER FROM dt)::int,
    EXTRACT(YEAR FROM dt)::int,
    CASE WHEN EXTRACT(DOW FROM dt)::int IN (0, 6) THEN 'Y' ELSE 'N' END
FROM (
    SELECT d_sk, pgbench.int_to_date('2000-01-01', d_sk - 1, 'day')::date AS dt
    FROM generate_series(:client_id + 1, :days_count, :num_workers) AS g(d_sk)
) sub;

-- 2. Item table (partitioned across workers by i_item_sk)
INSERT INTO item (
    i_item_sk, i_item_id, i_rec_start_date, i_rec_end_date, i_item_desc,
    i_current_price, i_wholesale_cost, i_brand_id, i_brand,
    i_class_id, i_class, i_category_id, i_category, i_manufact_id, i_manufact
)
SELECT
    i_sk,
    pgbench.format_string('ITEM%012d', i_sk)::char(16),
    '1999-01-01'::date,
    '2005-12-31'::date,
    pgbench.random_astring(25, 80),
    (pgbench.random_gaussian(200, 9500, 2.5) / 100.0)::numeric(7,2),
    (pgbench.random_gaussian(100, 6000, 2.5) / 100.0)::numeric(7,2),
    ((i_sk - 1) % 20) + 1,
    pgbench.format_string('Brand#%02d', ((i_sk - 1) % 20) + 1)::char(50),
    ((i_sk - 1) % 10) + 1,
    pgbench.format_string('Class#%02d', ((i_sk - 1) % 10) + 1)::char(50),
    ((i_sk - 1) % 6) + 1,
    CASE i_sk % 6
        WHEN 0 THEN 'Electronics'
        WHEN 1 THEN 'Women'
        WHEN 2 THEN 'Men'
        WHEN 3 THEN 'Home'
        WHEN 4 THEN 'Sports'
        ELSE 'Books'
    END::char(50),
    ((i_sk - 1) % 15) + 1,
    pgbench.format_string('Manufact#%02d', ((i_sk - 1) % 15) + 1)::char(50)
FROM generate_series(:client_id + 1, :items_per_scale * :scale, :num_workers) AS g(i_sk);

-- 3. Customer-Address table (partitioned across workers)
INSERT INTO customer_address (
    ca_address_sk, ca_address_id, ca_street_name, ca_city, ca_county,
    ca_state, ca_zip, ca_country, ca_gmt_offset
)
SELECT
    ca_sk,
    pgbench.format_string('ADDR%012d', ca_sk)::char(16),
    pgbench.random_astring(10, 25),
    pgbench.format_string('City-%03d', (ca_sk % 50) + 1),
    pgbench.format_string('County-%02d', (ca_sk % 20) + 1),
    CASE ca_sk % 10
        WHEN 0 THEN 'CA' WHEN 1 THEN 'NY' WHEN 2 THEN 'TX' WHEN 3 THEN 'WA' WHEN 4 THEN 'FL'
        WHEN 5 THEN 'IL' WHEN 6 THEN 'PA' WHEN 7 THEN 'OH' WHEN 8 THEN 'GA' ELSE 'NC'
    END,
    pgbench.random_nstring(5, 5) || '     ',
    'United States',
    -5.00
FROM generate_series(:client_id + 1, :cust_per_scale * :scale, :num_workers) AS g(ca_sk);

-- 4. Customer-Demographics table (partitioned across workers)
INSERT INTO customer_demographics (
    cd_demo_sk, cd_gender, cd_marital_status, cd_education_status,
    cd_purchase_estimate, cd_credit_rating, cd_dep_count
)
SELECT
    cd_sk,
    CASE cd_sk % 2 WHEN 0 THEN 'M' ELSE 'F' END,
    CASE cd_sk % 3 WHEN 0 THEN 'M' WHEN 1 THEN 'S' ELSE 'D' END,
    CASE cd_sk % 4
        WHEN 0 THEN 'Primary'
        WHEN 1 THEN 'Secondary'
        WHEN 2 THEN 'College'
        ELSE 'Advanced Degree'
    END::char(20),
    ((cd_sk % 10) + 1) * 1000,
    CASE cd_sk % 3 WHEN 0 THEN 'Good' WHEN 1 THEN 'Low Risk' ELSE 'High Risk' END::char(10),
    (cd_sk % 5)::int
FROM generate_series(:client_id + 1, :cust_per_scale * :scale, :num_workers) AS g(cd_sk);

-- 5. Customer table (partitioned across workers)
INSERT INTO customer (
    c_customer_sk, c_customer_id, c_current_cdemo_sk, c_current_addr_sk,
    c_first_name, c_last_name, c_preferred_cust_flag,
    c_birth_year, c_birth_country, c_email_address
)
SELECT
    c_sk,
    pgbench.format_string('CUST%012d', c_sk)::char(16),
    c_sk,
    c_sk,
    pgbench.random_astring(6, 12)::char(20),
    pgbench.c_last((c_sk - 1) % 1000)::char(30),
    CASE WHEN c_sk % 3 = 0 THEN 'Y' ELSE 'N' END,
    1945 + (c_sk % 50),
    'UNITED STATES',
    pgbench.format_string('user%06d@tpcds.example.org', c_sk)::char(50)
FROM generate_series(:client_id + 1, :cust_per_scale * :scale, :num_workers) AS g(c_sk);

-- 6. Store and Warehouse tables (partitioned across workers)
INSERT INTO store (
    s_store_sk, s_store_id, s_store_name, s_number_employees,
    s_floor_space, s_city, s_state, s_tax_percentage
)
SELECT
    s_sk,
    pgbench.format_string('STORE%011d', s_sk)::char(16),
    pgbench.format_string('Store-%04d', s_sk),
    50 + (s_sk * 10),
    10000 + (s_sk * 500),
    pgbench.format_string('City-%02d', s_sk),
    CASE s_sk % 5 WHEN 0 THEN 'CA' WHEN 1 THEN 'NY' WHEN 2 THEN 'TX' WHEN 3 THEN 'WA' ELSE 'TN' END,
    0.07
FROM generate_series(:client_id + 1, GREATEST(6 * :scale, 6), :num_workers) AS g(s_sk);

INSERT INTO warehouse (
    w_warehouse_sk, w_warehouse_id, w_warehouse_name,
    w_warehouse_sq_ft, w_city, w_state
)
SELECT
    w_sk,
    pgbench.format_string('WH%014d', w_sk)::char(16),
    pgbench.format_string('Warehouse-%02d', w_sk),
    50000 + (w_sk * 5000),
    pgbench.format_string('City-%02d', w_sk),
    CASE w_sk % 5 WHEN 0 THEN 'CA' WHEN 1 THEN 'NY' WHEN 2 THEN 'TX' WHEN 3 THEN 'WA' ELSE 'TN' END
FROM generate_series(:client_id + 1, GREATEST(4 * :scale, 4), :num_workers) AS g(w_sk);

-- 7. Promotion table (partitioned across workers)
INSERT INTO promotion (
    p_promo_sk, p_promo_id, p_item_sk, p_cost, p_response_target,
    p_promo_name, p_channel_dmail, p_channel_email, p_channel_tv
)
SELECT
    p_sk,
    pgbench.format_string('PROMO%011d', p_sk)::char(16),
    ((p_sk - 1) % (:items_per_scale * :scale)) + 1,
    1000.00,
    1,
    pgbench.format_string('Promo-%04d', p_sk)::char(50),
    CASE WHEN p_sk % 2 = 0 THEN 'Y' ELSE 'N' END,
    CASE WHEN p_sk % 3 = 0 THEN 'Y' ELSE 'N' END,
    CASE WHEN p_sk % 4 = 0 THEN 'Y' ELSE 'N' END
FROM generate_series(:client_id + 1, GREATEST(20 * :scale, 20), :num_workers) AS g(p_sk);

-- 8. Store-Sales and Store-Returns fact tables (partitioned across workers)
INSERT INTO store_sales (
    ss_sold_date_sk, ss_item_sk, ss_customer_sk, ss_cdemo_sk, ss_addr_sk,
    ss_store_sk, ss_promo_sk, ss_ticket_number, ss_quantity,
    ss_wholesale_cost, ss_list_price, ss_sales_price,
    ss_ext_discount_amt, ss_ext_sales_price, ss_ext_wholesale_cost,
    ss_ext_list_price, ss_ext_tax, ss_coupon_amt, ss_net_paid, ss_net_profit
)
SELECT
    ((t_num - 1) % :days_count) + 1,
    pgbench.random_zipfian(1, :items_per_scale * :scale, 1.15),
    pgbench.random_zipfian(1, :cust_per_scale * :scale, 1.15),
    ((t_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((t_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((t_num - 1) % GREATEST(6 * :scale, 6)) + 1,
    ((t_num - 1) % GREATEST(20 * :scale, 20)) + 1,
    t_num,
    qty,
    20.00,
    40.00,
    35.00,
    (qty * 5.00)::numeric(7,2),
    (qty * 35.00)::numeric(7,2),
    (qty * 20.00)::numeric(7,2),
    (qty * 40.00)::numeric(7,2),
    (qty * 2.45)::numeric(7,2),
    0.00,
    (qty * 35.00)::numeric(7,2),
    (qty * 15.00)::numeric(7,2)
FROM (
    SELECT t_num, pgbench.random_gaussian(1, 20, 2.5)::int AS qty
    FROM generate_series(:client_id + 1, :sales_per_scale * :scale, :num_workers) AS g(t_num)
) sub;

INSERT INTO store_returns (
    sr_returned_date_sk, sr_item_sk, sr_customer_sk, sr_cdemo_sk, sr_store_sk,
    sr_ticket_number, sr_return_quantity, sr_return_amt, sr_return_tax,
    sr_return_amt_inc_tax, sr_fee, sr_return_ship_cost, sr_refunded_cash, sr_net_loss
)
SELECT
    ((t_num + 5 - 1) % :days_count) + 1,
    ((t_num - 1) % (:items_per_scale * :scale)) + 1,
    ((t_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((t_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((t_num - 1) % GREATEST(6 * :scale, 6)) + 1,
    t_num,
    2,
    70.00,
    4.90,
    74.90,
    5.00,
    3.50,
    70.00,
    13.40
FROM generate_series(:client_id + 1, (:sales_per_scale * :scale) / 5, :num_workers) AS g(t_num);

-- 9. Catalog-Sales and Web-Sales fact tables (partitioned across workers)
INSERT INTO catalog_sales (
    cs_sold_date_sk, cs_ship_date_sk, cs_bill_customer_sk, cs_bill_cdemo_sk,
    cs_ship_addr_sk, cs_warehouse_sk, cs_item_sk, cs_promo_sk, cs_order_number,
    cs_quantity, cs_wholesale_cost, cs_list_price, cs_sales_price,
    cs_ext_sales_price, cs_net_paid, cs_net_profit
)
SELECT
    ((o_num - 1) % :days_count) + 1,
    ((o_num + 3 - 1) % :days_count) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % GREATEST(4 * :scale, 4)) + 1,
    pgbench.random_zipfian(1, :items_per_scale * :scale, 1.15),
    ((o_num - 1) % GREATEST(20 * :scale, 20)) + 1,
    o_num,
    qty,
    22.00,
    45.00,
    38.00,
    (qty * 38.00)::numeric(7,2),
    (qty * 38.00)::numeric(7,2),
    (qty * 16.00)::numeric(7,2)
FROM (
    SELECT o_num, pgbench.random(1, 15)::int AS qty
    FROM generate_series(:client_id + 1, (:sales_per_scale * :scale) / 2, :num_workers) AS g(o_num)
) sub;

INSERT INTO web_sales (
    ws_sold_date_sk, ws_ship_date_sk, ws_bill_customer_sk, ws_bill_cdemo_sk,
    ws_ship_addr_sk, ws_warehouse_sk, ws_item_sk, ws_promo_sk, ws_order_number,
    ws_quantity, ws_wholesale_cost, ws_list_price, ws_sales_price,
    ws_ext_sales_price, ws_net_paid, ws_net_profit
)
SELECT
    ((o_num - 1) % :days_count) + 1,
    ((o_num + 2 - 1) % :days_count) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % (:cust_per_scale * :scale)) + 1,
    ((o_num - 1) % GREATEST(4 * :scale, 4)) + 1,
    pgbench.random_zipfian(1, :items_per_scale * :scale, 1.15),
    ((o_num - 1) % GREATEST(20 * :scale, 20)) + 1,
    o_num,
    qty,
    18.00,
    36.00,
    30.00,
    (qty * 30.00)::numeric(7,2),
    (qty * 30.00)::numeric(7,2),
    (qty * 12.00)::numeric(7,2)
FROM (
    SELECT o_num, pgbench.random(1, 15)::int AS qty
    FROM generate_series(:client_id + 1, (:sales_per_scale * :scale) / 2, :num_workers) AS g(o_num)
) sub;

-- 10. Inventory fact table (weekly snapshots across items and warehouses)
INSERT INTO inventory (inv_date_sk, inv_item_sk, inv_warehouse_sk, inv_quantity_on_hand)
SELECT
    (wk - 1) * 7 + 1,
    i_sk,
    w_sk,
    pgbench.random_gaussian(10, 500, 2.5)::int
FROM generate_series(:client_id + 1, :items_per_scale * :scale, :num_workers) AS i(i_sk)
CROSS JOIN generate_series(1, GREATEST(4 * :scale, 4)) AS w(w_sk)
CROSS JOIN generate_series(1, :days_count / 30) AS d(wk);
