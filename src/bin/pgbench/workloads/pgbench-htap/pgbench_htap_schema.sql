--
-- CH-benCHmark (HTAP: TPC-C + TPC-H) Schema and Stored Procedures for pgbench
--
-- Combines the 9 TPC-C operational tables and 5 transactional stored
-- procedures with the 3 CH-benCHmark analytical dimension tables
-- (htap_region, htap_nation, htap_supplier).
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS htap_supplier CASCADE;
DROP TABLE IF EXISTS htap_nation CASCADE;
DROP TABLE IF EXISTS htap_region CASCADE;
DROP TABLE IF EXISTS order_line CASCADE;
DROP TABLE IF EXISTS new_order CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS history CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS stock CASCADE;
DROP TABLE IF EXISTS item CASCADE;
DROP TABLE IF EXISTS district CASCADE;
DROP TABLE IF EXISTS warehouse CASCADE;

CREATE TABLE warehouse (
    w_id        integer PRIMARY KEY,
    w_name      varchar(10) NOT NULL,
    w_street_1  varchar(20) NOT NULL,
    w_street_2  varchar(20) NOT NULL,
    w_city      varchar(20) NOT NULL,
    w_state     char(2) NOT NULL,
    w_zip       char(9) NOT NULL,
    w_tax       numeric(4,4) NOT NULL,
    w_ytd       numeric(12,2) NOT NULL
);

CREATE TABLE district (
    d_id        smallint NOT NULL,
    d_w_id      integer NOT NULL,
    d_name      varchar(10) NOT NULL,
    d_street_1  varchar(20) NOT NULL,
    d_street_2  varchar(20) NOT NULL,
    d_city      varchar(20) NOT NULL,
    d_state     char(2) NOT NULL,
    d_zip       char(9) NOT NULL,
    d_tax       numeric(4,4) NOT NULL,
    d_ytd       numeric(12,2) NOT NULL,
    d_next_o_id integer NOT NULL,
    PRIMARY KEY (d_w_id, d_id)
);

CREATE TABLE customer (
    c_id           integer NOT NULL,
    c_d_id         smallint NOT NULL,
    c_w_id         integer NOT NULL,
    c_first        varchar(16) NOT NULL,
    c_middle       char(2) NOT NULL,
    c_last         varchar(16) NOT NULL,
    c_street_1     varchar(20) NOT NULL,
    c_street_2     varchar(20) NOT NULL,
    c_city         varchar(20) NOT NULL,
    c_state        char(2) NOT NULL,
    c_zip          char(9) NOT NULL,
    c_phone        char(16) NOT NULL,
    c_since        timestamp NOT NULL,
    c_credit       char(2) NOT NULL,
    c_credit_lim   numeric(12,2) NOT NULL,
    c_discount     numeric(4,4) NOT NULL,
    c_balance      numeric(12,2) NOT NULL,
    c_ytd_payment  numeric(12,2) NOT NULL,
    c_payment_cnt  integer NOT NULL,
    c_delivery_cnt integer NOT NULL,
    c_data         varchar(500) NOT NULL,
    PRIMARY KEY (c_w_id, c_d_id, c_id)
);

CREATE TABLE history (
    h_c_id   integer NOT NULL,
    h_c_d_id smallint NOT NULL,
    h_c_w_id integer NOT NULL,
    h_d_id   smallint NOT NULL,
    h_w_id   integer NOT NULL,
    h_date   timestamp NOT NULL,
    h_amount numeric(6,2) NOT NULL,
    h_data   varchar(24) NOT NULL
);

CREATE TABLE new_order (
    no_o_id integer NOT NULL,
    no_d_id smallint NOT NULL,
    no_w_id integer NOT NULL,
    PRIMARY KEY (no_w_id, no_d_id, no_o_id)
);

CREATE TABLE orders (
    o_id         integer NOT NULL,
    o_d_id       smallint NOT NULL,
    o_w_id       integer NOT NULL,
    o_c_id       integer NOT NULL,
    o_entry_d    timestamp NOT NULL,
    o_carrier_id smallint,
    o_ol_cnt     smallint NOT NULL,
    o_all_local  smallint NOT NULL,
    PRIMARY KEY (o_w_id, o_d_id, o_id)
);

CREATE TABLE order_line (
    ol_o_id        integer NOT NULL,
    ol_d_id        smallint NOT NULL,
    ol_w_id        integer NOT NULL,
    ol_number      smallint NOT NULL,
    ol_i_id        integer NOT NULL,
    ol_supply_w_id integer NOT NULL,
    ol_delivery_d  timestamp,
    ol_quantity    smallint NOT NULL,
    ol_amount      numeric(6,2) NOT NULL,
    ol_dist_info   char(24) NOT NULL,
    PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number)
);

CREATE TABLE item (
    i_id    integer PRIMARY KEY,
    i_im_id integer NOT NULL,
    i_name  varchar(24) NOT NULL,
    i_price numeric(5,2) NOT NULL,
    i_data  varchar(50) NOT NULL
);

CREATE TABLE stock (
    s_i_id       integer NOT NULL,
    s_w_id       integer NOT NULL,
    s_quantity   smallint NOT NULL,
    s_dist_01    char(24) NOT NULL,
    s_dist_02    char(24) NOT NULL,
    s_dist_03    char(24) NOT NULL,
    s_dist_04    char(24) NOT NULL,
    s_dist_05    char(24) NOT NULL,
    s_dist_06    char(24) NOT NULL,
    s_dist_07    char(24) NOT NULL,
    s_dist_08    char(24) NOT NULL,
    s_dist_09    char(24) NOT NULL,
    s_dist_10    char(24) NOT NULL,
    s_ytd        integer NOT NULL,
    s_order_cnt  integer NOT NULL,
    s_remote_cnt integer NOT NULL,
    s_data       varchar(50) NOT NULL,
    PRIMARY KEY (s_w_id, s_i_id)
);

CREATE TABLE htap_region (
    r_regionkey integer PRIMARY KEY,
    r_name      char(25) NOT NULL,
    r_comment   varchar(152)
);

CREATE TABLE htap_nation (
    n_nationkey integer PRIMARY KEY,
    n_name      char(25) NOT NULL,
    n_regionkey integer NOT NULL,
    n_comment   varchar(152)
);

CREATE TABLE htap_supplier (
    su_suppkey   integer PRIMARY KEY,
    su_name      char(25) NOT NULL,
    su_address   varchar(40) NOT NULL,
    su_nationkey integer NOT NULL,
    su_phone     char(15) NOT NULL,
    su_acctbal   numeric(12,2) NOT NULL,
    su_comment   varchar(101) NOT NULL
);

CREATE OR REPLACE PROCEDURE pgbench_c_new_order(
    p_w_id    integer,
    p_d_id    integer,
    p_c_id    integer,
    p_ol_cnt  integer,
    p_i_ids   integer[],
    p_s_w_ids integer[],
    p_qtys    integer[]
) LANGUAGE plpgsql AS $$
DECLARE
    v_w_tax      numeric(4,4);
    v_d_tax      numeric(4,4);
    v_o_id       integer;
    v_c_discount numeric(4,4);
    v_c_last     varchar(16);
    v_c_credit   char(2);
    v_all_local  smallint := 1;
    v_i_price    numeric(5,2);
    v_i_name     varchar(24);
    v_i_data     varchar(50);
    v_s_quantity smallint;
    v_s_data     varchar(50);
    v_dist_info  char(24);
    v_ol_amount  numeric(6,2);
    v_remote_inc integer;
    idx          integer;
BEGIN
    SELECT w_tax INTO v_w_tax FROM warehouse WHERE w_id = p_w_id;

    UPDATE district
       SET d_next_o_id = d_next_o_id + 1
     WHERE d_w_id = p_w_id AND d_id = p_d_id
    RETURNING d_next_o_id - 1, d_tax INTO v_o_id, v_d_tax;

    SELECT c_discount, c_last, c_credit
      INTO v_c_discount, v_c_last, v_c_credit
      FROM customer
     WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = p_c_id;

    FOR idx IN 1 .. p_ol_cnt LOOP
        IF p_s_w_ids[idx] <> p_w_id THEN
            v_all_local := 0;
        END IF;
    END LOOP;

    INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local)
    VALUES (v_o_id, p_d_id, p_w_id, p_c_id, CURRENT_TIMESTAMP, NULL, p_ol_cnt, v_all_local);

    INSERT INTO new_order (no_o_id, no_d_id, no_w_id)
    VALUES (v_o_id, p_d_id, p_w_id);

    FOR idx IN 1 .. p_ol_cnt LOOP
        SELECT i_price, i_name, i_data
          INTO v_i_price, v_i_name, v_i_data
          FROM item
         WHERE i_id = p_i_ids[idx];

        IF NOT FOUND THEN
            RAISE EXCEPTION 'Invalid item ID: %', p_i_ids[idx]
                USING ERRCODE = 'P0001';
        END IF;

        v_remote_inc := CASE WHEN p_s_w_ids[idx] <> p_w_id THEN 1 ELSE 0 END;

        UPDATE stock
           SET s_quantity = CASE
                                WHEN s_quantity - p_qtys[idx] >= 10
                                THEN s_quantity - p_qtys[idx]
                                ELSE s_quantity - p_qtys[idx] + 91
                            END,
               s_ytd = s_ytd + p_qtys[idx],
               s_order_cnt = s_order_cnt + 1,
               s_remote_cnt = s_remote_cnt + v_remote_inc
         WHERE s_w_id = p_s_w_ids[idx] AND s_i_id = p_i_ids[idx]
        RETURNING s_quantity, s_data, s_dist_01
          INTO v_s_quantity, v_s_data, v_dist_info;

        v_ol_amount := round(p_qtys[idx] * v_i_price * (1 + v_w_tax + v_d_tax) * (1 - v_c_discount), 2);

        INSERT INTO order_line (
            ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id,
            ol_supply_w_id, ol_delivery_d, ol_quantity, ol_amount, ol_dist_info
        ) VALUES (
            v_o_id, p_d_id, p_w_id, idx, p_i_ids[idx],
            p_s_w_ids[idx], NULL, p_qtys[idx], v_ol_amount, COALESCE(v_dist_info, 'DIST_INFO               ')
        );
    END LOOP;
END;
$$;

CREATE OR REPLACE PROCEDURE pgbench_c_payment(
    p_w_id     integer,
    p_d_id     integer,
    p_c_w_id   integer,
    p_c_d_id   integer,
    p_c_id     integer,
    p_c_last   text,
    p_h_amount numeric
) LANGUAGE plpgsql AS $$
DECLARE
    v_w_name varchar(10);
    v_d_name varchar(10);
    v_c_id   integer;
BEGIN
    UPDATE warehouse SET w_ytd = w_ytd + p_h_amount WHERE w_id = p_w_id RETURNING w_name INTO v_w_name;
    UPDATE district SET d_ytd = d_ytd + p_h_amount WHERE d_w_id = p_w_id AND d_id = p_d_id RETURNING d_name INTO v_d_name;

    IF p_c_id IS NOT NULL THEN
        v_c_id := p_c_id;
    ELSE
        SELECT c_id INTO v_c_id
          FROM customer
         WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id
         ORDER BY c_last = p_c_last DESC, c_last >= p_c_last DESC, c_last, c_first
         LIMIT 1;
    END IF;

    UPDATE customer
       SET c_balance = c_balance - p_h_amount,
           c_ytd_payment = c_ytd_payment + p_h_amount,
           c_payment_cnt = c_payment_cnt + 1
     WHERE c_w_id = p_c_w_id AND c_d_id = p_c_d_id AND c_id = v_c_id;

    INSERT INTO history (h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data)
    VALUES (v_c_id, p_c_d_id, p_c_w_id, p_d_id, p_w_id, CURRENT_TIMESTAMP,
            p_h_amount, left(v_w_name || '    ' || v_d_name, 24));
END;
$$;

CREATE OR REPLACE PROCEDURE pgbench_c_order_status(
    p_w_id   integer,
    p_d_id   integer,
    p_c_id   integer,
    p_c_last text
) LANGUAGE plpgsql AS $$
DECLARE
    v_c_id integer;
    v_o_id integer;
BEGIN
    IF p_c_id IS NOT NULL THEN
        v_c_id := p_c_id;
    ELSE
        SELECT c_id INTO v_c_id
          FROM customer
         WHERE c_w_id = p_w_id AND c_d_id = p_d_id
         ORDER BY c_last = p_c_last DESC, c_last >= p_c_last DESC, c_last, c_first
         LIMIT 1;
    END IF;

    SELECT o_id INTO v_o_id
      FROM orders
     WHERE o_w_id = p_w_id AND o_d_id = p_d_id AND o_c_id = v_c_id
     ORDER BY o_id DESC
     LIMIT 1;
END;
$$;

CREATE OR REPLACE PROCEDURE pgbench_c_delivery(
    p_w_id         integer,
    p_d_id         integer,
    p_o_carrier_id integer
) LANGUAGE plpgsql AS $$
DECLARE
    v_o_id     integer;
    v_c_id     integer;
    v_ol_total numeric(12,2);
BEGIN
    DELETE FROM new_order
     WHERE no_w_id = p_w_id
       AND no_d_id = p_d_id
       AND no_o_id = (
           SELECT min(no_o_id)
             FROM new_order
            WHERE no_w_id = p_w_id AND no_d_id = p_d_id
       )
    RETURNING no_o_id INTO v_o_id;

    IF v_o_id IS NOT NULL THEN
        UPDATE orders
           SET o_carrier_id = p_o_carrier_id
         WHERE o_w_id = p_w_id AND o_d_id = p_d_id AND o_id = v_o_id
        RETURNING o_c_id INTO v_c_id;

        UPDATE order_line
           SET ol_delivery_d = CURRENT_TIMESTAMP
         WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id;

        SELECT COALESCE(sum(ol_amount), 0) INTO v_ol_total
          FROM order_line
         WHERE ol_w_id = p_w_id AND ol_d_id = p_d_id AND ol_o_id = v_o_id;

        UPDATE customer
           SET c_balance = c_balance + v_ol_total,
               c_delivery_cnt = c_delivery_cnt + 1
         WHERE c_w_id = p_w_id AND c_d_id = p_d_id AND c_id = v_c_id;
    END IF;
END;
$$;

CREATE OR REPLACE PROCEDURE pgbench_c_stock_level(
    p_w_id      integer,
    p_d_id      integer,
    p_threshold integer,
    INOUT p_low_stock integer DEFAULT 0
) LANGUAGE plpgsql AS $$
DECLARE
    v_next_o_id integer;
BEGIN
    SELECT d_next_o_id INTO v_next_o_id
      FROM district
     WHERE d_w_id = p_w_id AND d_id = p_d_id;

    SELECT count(DISTINCT s_i_id)
      INTO p_low_stock
      FROM order_line
      JOIN stock ON s_w_id = ol_w_id AND s_i_id = ol_i_id
     WHERE ol_w_id = p_w_id
       AND ol_d_id = p_d_id
       AND ol_o_id >= v_next_o_id - 20
       AND ol_o_id < v_next_o_id
       AND s_quantity < p_threshold;
END;
$$;
