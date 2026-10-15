--
-- pgbench-b (modernized, database-function based workload)
-- Schema DDL and Server-Side Stored Procedures
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS pgbench_history CASCADE;
DROP TABLE IF EXISTS pgbench_accounts CASCADE;
DROP TABLE IF EXISTS pgbench_tellers CASCADE;
DROP TABLE IF EXISTS pgbench_branches CASCADE;

CREATE TABLE pgbench_branches (
    bid integer NOT NULL,
    bbalance integer DEFAULT 0,
    filler char(88)
);

CREATE TABLE pgbench_tellers (
    tid integer NOT NULL,
    bid integer,
    tbalance integer DEFAULT 0,
    filler char(84)
);

CREATE TABLE pgbench_accounts (
    aid integer NOT NULL,
    bid integer,
    abalance integer DEFAULT 0,
    filler char(84)
);

CREATE TABLE pgbench_history (
    tid integer,
    bid integer,
    aid integer,
    delta integer,
    mtime timestamp,
    filler char(22)
);

--
-- Modernized banking transaction procedure
--
-- Executes the classic 5-statement transaction inside a single database
-- procedure call, eliminating client-server roundtrips and running cleanly
-- under PostgreSQL autocommit mode.
--
CREATE OR REPLACE PROCEDURE pgbench_b_transaction(
    p_aid integer,
    p_bid integer,
    p_tid integer,
    p_delta integer,
    INOUT p_abalance integer DEFAULT 0
)
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE pgbench_accounts
       SET abalance = abalance + p_delta
     WHERE aid = p_aid
 RETURNING abalance INTO p_abalance;

    UPDATE pgbench_tellers
       SET tbalance = tbalance + p_delta
     WHERE tid = p_tid;

    UPDATE pgbench_branches
       SET bbalance = bbalance + p_delta
     WHERE bid = p_bid;

    INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
    VALUES (p_tid, p_bid, p_aid, p_delta, clock_timestamp());
END;
$$;

--
-- Modernized Simple-Update transaction procedure
--
CREATE OR REPLACE PROCEDURE pgbench_b_simple_update(
    p_aid integer,
    p_bid integer,
    p_tid integer,
    p_delta integer,
    INOUT p_abalance integer DEFAULT 0
)
LANGUAGE plpgsql
AS $$
BEGIN
    UPDATE pgbench_accounts
       SET abalance = abalance + p_delta
     WHERE aid = p_aid
 RETURNING abalance INTO p_abalance;

    INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
    VALUES (p_tid, p_bid, p_aid, p_delta, clock_timestamp());
END;
$$;
