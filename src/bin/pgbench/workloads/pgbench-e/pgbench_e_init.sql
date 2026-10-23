--
-- pgbench-e (TPC-E-inspired) Parallel In-Database Data Generation Script for pgbench
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> -f pgbench_e_init.sql
--
-- Optional density overrides (via -D):
--   brokers_per_scale    (default: 10)
--   companies_per_scale  (default: 50)
--   securities_per_scale (default: 100)
--   customers_per_scale  (default: 100)
--   trades_per_scale     (default: 300)
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':brokers_per_scale' FROM '^[0-9]+$')::int, 10) AS brokers_per_scale,
    COALESCE(substring(':companies_per_scale' FROM '^[0-9]+$')::int, 50) AS companies_per_scale,
    COALESCE(substring(':securities_per_scale' FROM '^[0-9]+$')::int, 100) AS securities_per_scale,
    COALESCE(substring(':customers_per_scale' FROM '^[0-9]+$')::int, 100) AS customers_per_scale,
    COALESCE(substring(':trades_per_scale' FROM '^[0-9]+$')::int, 300) AS trades_per_scale
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Trade-Type reference table (idempotent across workers)
INSERT INTO trade_type (tt_id, tt_name, tt_is_sell, tt_is_mrkt)
VALUES
    ('TMB', 'Market-Buy',  false, true),
    ('TMS', 'Market-Sell', true,  true),
    ('TLB', 'Limit-Buy',   false, false),
    ('TLS', 'Limit-Sell',  true,  false),
    ('TSL', 'Stop-Loss',   true,  false)
ON CONFLICT (tt_id) DO NOTHING;

-- 2. Broker table (partitioned across workers)
INSERT INTO broker (b_id, b_st_id, b_name, b_num_trades, b_comm_total)
SELECT
    b_id,
    'ACTV',
    pgbench.format_string('Broker-%06d-%s', b_id, pgbench.random_astring(4, 10)),
    pgbench.random(10, 500)::int,
    (pgbench.random(1000, 500000) / 100.0)::numeric(12,2)
FROM generate_series(:client_id + 1, :brokers_per_scale * :scale, :num_workers) AS g(b_id);

-- 3. Company table (partitioned across workers)
INSERT INTO company (co_id, co_st_id, co_name, co_in_id, co_sp_rate, co_ceo, co_desc, co_open_date)
SELECT
    co_id,
    'ACTV',
    pgbench.format_string('Company-%06d-%s', co_id, pgbench.random_astring(6, 12)),
    pgbench.format_string('%02d', (co_id % 20) + 1),
    CASE co_id % 4 WHEN 0 THEN 'AAA ' WHEN 1 THEN 'AA+ ' WHEN 2 THEN 'A   ' ELSE 'BBB ' END,
    pgbench.format_string('CEO-%06d %s', co_id, pgbench.random_astring(5, 10)),
    pgbench.random_astring(25, 60),
    pgbench.date_add('1990-01-01', (co_id * 17) % 7000, 'day')::date
FROM generate_series(:client_id + 1, :companies_per_scale * :scale, :num_workers) AS g(co_id);

-- 4. Security table (partitioned across workers)
INSERT INTO security (
    s_symb, s_issue, s_st_id, s_name, s_ex_id, s_co_id, s_num_out,
    s_start_date, s_exch_date, s_pe, s_52wk_high, s_52wk_high_date,
    s_52wk_low, s_52wk_low_date, s_dividend, s_yield
)
SELECT
    pgbench.format_string('SYM%06d', s_idx)::char(15),
    'COMMON',
    'ACTV',
    pgbench.format_string('Security %06d %s', s_idx, pgbench.random_astring(6, 14)),
    CASE s_idx % 2 WHEN 0 THEN 'NYSE  ' ELSE 'NASDAQ' END,
    ((s_idx - 1) % (:companies_per_scale * :scale)) + 1,
    1000000 + (s_idx * 1000),
    pgbench.date_add('1995-01-01', s_idx % 3000, 'day')::date,
    pgbench.date_add('1995-01-01', s_idx % 3000, 'day')::date,
    (pgbench.random_gaussian(500, 4500, 3.0) / 100.0)::numeric(10,2),
    (pgbench.random(5000, 9500) / 100.0)::numeric(8,2),
    '2024-06-01'::date,
    (pgbench.random(1500, 4900) / 100.0)::numeric(8,2),
    '2024-01-15'::date,
    (pgbench.random(10, 400) / 100.0)::numeric(10,2),
    (pgbench.random(50, 600) / 100.0)::numeric(5,2)
FROM generate_series(:client_id + 1, :securities_per_scale * :scale, :num_workers) AS g(s_idx);

-- 5. Last-Trade table (partitioned across workers)
INSERT INTO last_trade (lt_s_symb, lt_dts, lt_price, lt_open_price, lt_vol)
SELECT
    pgbench.format_string('SYM%06d', s_idx)::char(15),
    pgbench.date_add('2024-01-01', s_idx % 180, 'day')::timestamp,
    (pgbench.random_gaussian(2000, 8000, 2.5) / 100.0)::numeric(8,2),
    (pgbench.random_gaussian(2000, 8000, 2.5) / 100.0)::numeric(8,2),
    pgbench.random(1000, 500000)
FROM generate_series(:client_id + 1, :securities_per_scale * :scale, :num_workers) AS g(s_idx);

-- 6. Customer table (partitioned across workers)
INSERT INTO customer (
    c_id, c_tax_id, c_st_id, c_l_name, c_f_name, c_m_name,
    c_gndr, c_tier, c_dob, c_ad_id, c_email_1
)
SELECT
    c_id,
    pgbench.format_string('TAX-%09d', c_id),
    'ACTV',
    pgbench.c_last((c_id - 1) % 1000),
    pgbench.random_astring(6, 12),
    chr(65 + (c_id % 26)),
    CASE c_id % 2 WHEN 0 THEN 'M' ELSE 'F' END,
    ((c_id % 3) + 1)::smallint,
    pgbench.date_add('1955-01-01', (c_id * 31) % 15000, 'day')::date,
    c_id,
    pgbench.format_string('cust%06d@example.com', c_id)
FROM generate_series(:client_id + 1, :customers_per_scale * :scale, :num_workers) AS g(c_id);

-- 7. Customer-Account table (2 accounts per customer, partitioned by c_id)
INSERT INTO customer_account (ca_id, ca_b_id, ca_c_id, ca_name, ca_tax_st, ca_bal)
SELECT
    (c_id - 1) * 2 + acct_idx,
    ((c_id + acct_idx - 1) % (:brokers_per_scale * :scale)) + 1,
    c_id,
    pgbench.format_string('Account-%06d-%d', c_id, acct_idx),
    (acct_idx % 3)::smallint,
    (pgbench.random_gaussian(100000, 5000000, 2.5) / 100.0)::numeric(12,2)
FROM generate_series(:client_id + 1, :customers_per_scale * :scale, :num_workers) AS g(c_id)
CROSS JOIN generate_series(1, 2) AS a(acct_idx);

-- 8. Watch-List and Watch-Item tables (partitioned by c_id)
INSERT INTO watch_list (wl_id, wl_c_id)
SELECT
    c_id,
    c_id
FROM generate_series(:client_id + 1, :customers_per_scale * :scale, :num_workers) AS g(c_id);

INSERT INTO watch_item (wi_wl_id, wi_s_symb)
SELECT
    c_id,
    pgbench.format_string('SYM%06d', ((c_id + item_off - 1) % (:securities_per_scale * :scale)) + 1)::char(15)
FROM generate_series(:client_id + 1, :customers_per_scale * :scale, :num_workers) AS g(c_id)
CROSS JOIN generate_series(1, 5) AS w(item_off);

-- 9. Initial Trades, Trade-History, Holdings, Holding-Summary, Settlement, Cash-Transaction
INSERT INTO trade (
    t_id, t_dts, t_st_id, t_tt_id, t_is_cash, t_s_symb, t_qty,
    t_bid_price, t_ca_id, t_exec_name, t_trade_price,
    t_chrg, t_comm, t_tax, t_lifo
)
SELECT
    t_id,
    pgbench.date_add('2024-01-01', t_id % 180, 'day')::timestamp,
    CASE WHEN t_id % 10 = 0 THEN 'SBMT' ELSE 'CMPT' END,
    CASE t_id % 4 WHEN 0 THEN 'TMB' WHEN 1 THEN 'TMS' WHEN 2 THEN 'TLB' ELSE 'TLS' END,
    true,
    pgbench.format_string('SYM%06d', pgbench.random_zipfian(1, :securities_per_scale * :scale, 1.2))::char(15),
    (1 + (t_id % 8)) * 100,
    35.50,
    ((t_id - 1) % (:customers_per_scale * :scale * 2)) + 1,
    pgbench.format_string('Trader-%06d', t_id),
    35.50,
    10.00,
    15.00,
    2.50,
    (t_id % 2 = 0)
FROM generate_series(:client_id + 1, :trades_per_scale * :scale, :num_workers) AS g(t_id);

INSERT INTO trade_history (th_t_id, th_dts, th_st_id)
SELECT
    t_id,
    pgbench.date_add('2024-01-01', t_id % 180, 'day')::timestamp,
    CASE WHEN t_id % 10 = 0 THEN 'SBMT' ELSE 'CMPT' END
FROM generate_series(:client_id + 1, :trades_per_scale * :scale, :num_workers) AS g(t_id);

INSERT INTO holding (h_t_id, h_ca_id, h_s_symb, h_dts, h_price, h_qty)
SELECT
    t_id,
    ((t_id - 1) % (:customers_per_scale * :scale * 2)) + 1,
    pgbench.format_string('SYM%06d', ((t_id - 1) % (:securities_per_scale * :scale)) + 1)::char(15),
    pgbench.date_add('2024-01-01', t_id % 180, 'day')::timestamp,
    35.50,
    100
FROM generate_series(:client_id + 1, :trades_per_scale * :scale, :num_workers) AS g(t_id);

INSERT INTO holding_summary (hs_ca_id, hs_s_symb, hs_qty)
SELECT
    ca_id,
    pgbench.format_string('SYM%06d', ((ca_id + s_off - 1) % (:securities_per_scale * :scale)) + 1)::char(15),
    200
FROM generate_series(:client_id + 1, :customers_per_scale * :scale * 2, :num_workers) AS g(ca_id)
CROSS JOIN generate_series(1, 3) AS s(s_off);

INSERT INTO settlement (se_t_id, se_cash_type, se_cash_due_date, se_amt)
SELECT
    t_id,
    'Cash Account',
    pgbench.date_add('2024-01-03', t_id % 180, 'day')::date,
    3525.00
FROM generate_series(:client_id + 1, :trades_per_scale * :scale, :num_workers) AS g(t_id);

INSERT INTO cash_transaction (ct_t_id, ct_dts, ct_amt, ct_name)
SELECT
    t_id,
    pgbench.date_add('2024-01-01', t_id % 180, 'day')::timestamp,
    3525.00,
    pgbench.format_string('Initial settlement %06d', t_id)
FROM generate_series(:client_id + 1, :trades_per_scale * :scale, :num_workers) AS g(t_id);
