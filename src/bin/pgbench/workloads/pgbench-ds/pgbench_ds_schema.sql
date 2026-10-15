--
-- TPC-DS Schema for pgbench
--
-- Requires contrib/pgbench extension for in-database parallel data generation.
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS inventory CASCADE;
DROP TABLE IF EXISTS web_sales CASCADE;
DROP TABLE IF EXISTS catalog_sales CASCADE;
DROP TABLE IF EXISTS store_returns CASCADE;
DROP TABLE IF EXISTS store_sales CASCADE;
DROP TABLE IF EXISTS promotion CASCADE;
DROP TABLE IF EXISTS warehouse CASCADE;
DROP TABLE IF EXISTS store CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS customer_demographics CASCADE;
DROP TABLE IF EXISTS customer_address CASCADE;
DROP TABLE IF EXISTS item CASCADE;
DROP TABLE IF EXISTS date_dim CASCADE;

CREATE TABLE date_dim (
    d_date_sk     integer PRIMARY KEY,
    d_date_id     char(16) NOT NULL,
    d_date        date NOT NULL,
    d_month_seq   integer NOT NULL,
    d_week_seq    integer NOT NULL,
    d_quarter_seq integer NOT NULL,
    d_year        integer NOT NULL,
    d_dow         integer NOT NULL,
    d_moy         integer NOT NULL,
    d_dom         integer NOT NULL,
    d_qoy         integer NOT NULL,
    d_fy_year     integer NOT NULL,
    d_weekend     char(1) NOT NULL
);

CREATE TABLE item (
    i_item_sk        bigint PRIMARY KEY,
    i_item_id        char(16) NOT NULL,
    i_rec_start_date date,
    i_rec_end_date   date,
    i_item_desc      varchar(200),
    i_current_price  numeric(7,2),
    i_wholesale_cost numeric(7,2),
    i_brand_id       integer,
    i_brand          char(50),
    i_class_id       integer,
    i_class          char(50),
    i_category_id    integer,
    i_category       char(50),
    i_manufact_id    integer,
    i_manufact       char(50)
);

CREATE TABLE customer_address (
    ca_address_sk  bigint PRIMARY KEY,
    ca_address_id  char(16) NOT NULL,
    ca_street_name varchar(60),
    ca_city        varchar(60),
    ca_county      varchar(30),
    ca_state       char(2),
    ca_zip         char(10),
    ca_country     varchar(20),
    ca_gmt_offset  numeric(5,2)
);

CREATE TABLE customer_demographics (
    cd_demo_sk           bigint PRIMARY KEY,
    cd_gender            char(1),
    cd_marital_status    char(1),
    cd_education_status  char(20),
    cd_purchase_estimate integer,
    cd_credit_rating     char(10),
    cd_dep_count         integer
);

CREATE TABLE customer (
    c_customer_sk          bigint PRIMARY KEY,
    c_customer_id          char(16) NOT NULL,
    c_current_cdemo_sk     bigint,
    c_current_addr_sk      bigint,
    c_first_name           char(20),
    c_last_name            char(30),
    c_preferred_cust_flag  char(1),
    c_birth_year           integer,
    c_birth_country        varchar(20),
    c_email_address        char(50)
);

CREATE TABLE store (
    s_store_sk         integer PRIMARY KEY,
    s_store_id         char(16) NOT NULL,
    s_store_name       varchar(50),
    s_number_employees integer,
    s_floor_space      integer,
    s_city             varchar(60),
    s_state            char(2),
    s_tax_percentage   numeric(5,2)
);

CREATE TABLE warehouse (
    w_warehouse_sk    integer PRIMARY KEY,
    w_warehouse_id    char(16) NOT NULL,
    w_warehouse_name  varchar(20),
    w_warehouse_sq_ft integer,
    w_city            varchar(60),
    w_state           char(2)
);

CREATE TABLE promotion (
    p_promo_sk        integer PRIMARY KEY,
    p_promo_id        char(16) NOT NULL,
    p_item_sk         bigint,
    p_cost            numeric(15,2),
    p_response_target integer,
    p_promo_name      char(50),
    p_channel_dmail   char(1),
    p_channel_email   char(1),
    p_channel_tv      char(1)
);

CREATE TABLE store_sales (
    ss_sold_date_sk       integer NOT NULL,
    ss_item_sk            bigint NOT NULL,
    ss_customer_sk        bigint,
    ss_cdemo_sk           bigint,
    ss_addr_sk            bigint,
    ss_store_sk           integer,
    ss_promo_sk           integer,
    ss_ticket_number      bigint NOT NULL,
    ss_quantity           integer,
    ss_wholesale_cost     numeric(7,2),
    ss_list_price         numeric(7,2),
    ss_sales_price        numeric(7,2),
    ss_ext_discount_amt   numeric(7,2),
    ss_ext_sales_price    numeric(7,2),
    ss_ext_wholesale_cost numeric(7,2),
    ss_ext_list_price     numeric(7,2),
    ss_ext_tax            numeric(7,2),
    ss_coupon_amt         numeric(7,2),
    ss_net_paid           numeric(7,2),
    ss_net_profit         numeric(7,2),
    PRIMARY KEY (ss_item_sk, ss_ticket_number)
);

CREATE TABLE store_returns (
    sr_returned_date_sk   integer NOT NULL,
    sr_item_sk            bigint NOT NULL,
    sr_customer_sk        bigint,
    sr_cdemo_sk           bigint,
    sr_store_sk           integer,
    sr_ticket_number      bigint NOT NULL,
    sr_return_quantity    integer,
    sr_return_amt         numeric(7,2),
    sr_return_tax         numeric(7,2),
    sr_return_amt_inc_tax numeric(7,2),
    sr_fee                numeric(7,2),
    sr_return_ship_cost   numeric(7,2),
    sr_refunded_cash      numeric(7,2),
    sr_net_loss           numeric(7,2),
    PRIMARY KEY (sr_item_sk, sr_ticket_number)
);

CREATE TABLE catalog_sales (
    cs_sold_date_sk       integer NOT NULL,
    cs_ship_date_sk       integer,
    cs_bill_customer_sk   bigint,
    cs_bill_cdemo_sk      bigint,
    cs_ship_addr_sk       bigint,
    cs_warehouse_sk       integer,
    cs_item_sk            bigint NOT NULL,
    cs_promo_sk           integer,
    cs_order_number       bigint NOT NULL,
    cs_quantity           integer,
    cs_wholesale_cost     numeric(7,2),
    cs_list_price         numeric(7,2),
    cs_sales_price        numeric(7,2),
    cs_ext_sales_price    numeric(7,2),
    cs_net_paid           numeric(7,2),
    cs_net_profit         numeric(7,2),
    PRIMARY KEY (cs_item_sk, cs_order_number)
);

CREATE TABLE web_sales (
    ws_sold_date_sk       integer NOT NULL,
    ws_ship_date_sk       integer,
    ws_bill_customer_sk   bigint,
    ws_bill_cdemo_sk      bigint,
    ws_ship_addr_sk       bigint,
    ws_warehouse_sk       integer,
    ws_item_sk            bigint NOT NULL,
    ws_promo_sk           integer,
    ws_order_number       bigint NOT NULL,
    ws_quantity           integer,
    ws_wholesale_cost     numeric(7,2),
    ws_list_price         numeric(7,2),
    ws_sales_price        numeric(7,2),
    ws_ext_sales_price    numeric(7,2),
    ws_net_paid           numeric(7,2),
    ws_net_profit         numeric(7,2),
    PRIMARY KEY (ws_item_sk, ws_order_number)
);

CREATE TABLE inventory (
    inv_date_sk          integer NOT NULL,
    inv_item_sk          bigint NOT NULL,
    inv_warehouse_sk     integer NOT NULL,
    inv_quantity_on_hand integer,
    PRIMARY KEY (inv_date_sk, inv_item_sk, inv_warehouse_sk)
);
