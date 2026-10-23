--
-- TPC-H Schema for pgbench
--
-- Requires contrib/pgbench extension for in-database parallel data generation.
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS lineitem CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS partsupp CASCADE;
DROP TABLE IF EXISTS part CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS supplier CASCADE;
DROP TABLE IF EXISTS nation CASCADE;
DROP TABLE IF EXISTS region CASCADE;

CREATE TABLE region (
    r_regionkey integer PRIMARY KEY,
    r_name      char(25) NOT NULL,
    r_comment   varchar(152)
);

CREATE TABLE nation (
    n_nationkey integer PRIMARY KEY,
    n_name      char(25) NOT NULL,
    n_regionkey integer NOT NULL,
    n_comment   varchar(152)
);

CREATE TABLE supplier (
    s_suppkey   bigint PRIMARY KEY,
    s_name      char(25) NOT NULL,
    s_address   varchar(40) NOT NULL,
    s_nationkey integer NOT NULL,
    s_phone     char(15) NOT NULL,
    s_acctbal   numeric(15,2) NOT NULL,
    s_comment   varchar(101) NOT NULL
);

CREATE TABLE customer (
    c_custkey    bigint PRIMARY KEY,
    c_name       varchar(25) NOT NULL,
    c_address    varchar(40) NOT NULL,
    c_nationkey  integer NOT NULL,
    c_phone      char(15) NOT NULL,
    c_acctbal    numeric(15,2) NOT NULL,
    c_mktsegment char(10) NOT NULL,
    c_comment    varchar(117) NOT NULL
);

CREATE TABLE part (
    p_partkey     bigint PRIMARY KEY,
    p_name        varchar(55) NOT NULL,
    p_mfgr        char(25) NOT NULL,
    p_brand       char(10) NOT NULL,
    p_type        varchar(25) NOT NULL,
    p_size        integer NOT NULL,
    p_container   char(10) NOT NULL,
    p_retailprice numeric(15,2) NOT NULL,
    p_comment     varchar(23) NOT NULL
);

CREATE TABLE partsupp (
    ps_partkey    bigint NOT NULL,
    ps_suppkey    bigint NOT NULL,
    ps_availqty   integer NOT NULL,
    ps_supplycost numeric(15,2) NOT NULL,
    ps_comment    varchar(199) NOT NULL,
    PRIMARY KEY (ps_partkey, ps_suppkey)
);

CREATE TABLE orders (
    o_orderkey      bigint PRIMARY KEY,
    o_custkey       bigint NOT NULL,
    o_orderstatus   char(1) NOT NULL,
    o_totalprice    numeric(15,2) NOT NULL,
    o_orderdate     date NOT NULL,
    o_orderpriority char(15) NOT NULL,
    o_clerk         char(15) NOT NULL,
    o_shippriority  integer NOT NULL,
    o_comment       varchar(79) NOT NULL
);

CREATE TABLE lineitem (
    l_orderkey      bigint NOT NULL,
    l_partkey       bigint NOT NULL,
    l_suppkey       bigint NOT NULL,
    l_linenumber    integer NOT NULL,
    l_quantity      numeric(15,2) NOT NULL,
    l_extendedprice numeric(15,2) NOT NULL,
    l_discount      numeric(15,2) NOT NULL,
    l_tax           numeric(15,2) NOT NULL,
    l_returnflag    char(1) NOT NULL,
    l_linestatus    char(1) NOT NULL,
    l_shipdate      date NOT NULL,
    l_commitdate    date NOT NULL,
    l_receiptdate   date NOT NULL,
    l_shipinstruct  char(25) NOT NULL,
    l_shipmode      char(10) NOT NULL,
    l_comment       varchar(44) NOT NULL,
    PRIMARY KEY (l_orderkey, l_linenumber)
);
