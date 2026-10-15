--
-- pgbench-b (modernized, database-function based workload)
-- Parallel In-Database Data Generation Script for pgbench
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> -f pgbench_b_init.sql
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Branches and Tellers (populated by worker 0)
\if :client_id = 0
  INSERT INTO pgbench_branches (bid, bbalance, filler)
  SELECT b, 0, pgbench.random_astring(88, 88)
    FROM generate_series(1, :scale) AS b;

  INSERT INTO pgbench_tellers (tid, bid, tbalance, filler)
  SELECT t, 1 + ((t - 1) / 10), 0, pgbench.random_astring(84, 84)
    FROM generate_series(1, 10 * :scale) AS t;
\endif

-- 2. Accounts (partitioned across all workers)
INSERT INTO pgbench_accounts (aid, bid, abalance, filler)
SELECT a,
       1 + ((a - 1) / 100000),
       0,
       pgbench.random_astring(84, 84)
  FROM generate_series(:client_id + 1, 100000 * :scale, :num_workers) AS a;
