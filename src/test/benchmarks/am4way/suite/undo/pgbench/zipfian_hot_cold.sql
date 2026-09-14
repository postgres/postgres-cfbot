--
-- Zipfian hot/cold pgbench script: skewed access pattern
--
-- Uses random_zipfian() to create realistic hot/cold access patterns.
-- A small set of "hot" rows receives the majority of operations, while
-- the rest of the table is cold.  This stresses buffer management and
-- UNDO's per-row overhead on hot rows while cold rows may need disk I/O.
--
-- Mix: 50% hot reads, 20% hot updates, 15% cold reads, 10% cold updates,
--      5% rollback (exercises UNDO on hot rows).
--
-- Use with: pgbench -f zipfian_hot_cold.sql
--

\set rnd random(1, 100)
\set hot_aid random_zipfian(1, 100000 * :scale, 1.2)
\set cold_aid random(1, 100000 * :scale)
\set delta random(-5000, 5000)

BEGIN;

\if :rnd <= 50
-- Hot read (50%): Zipfian-distributed point lookup
SELECT abalance FROM pgbench_accounts WHERE aid = :hot_aid;
COMMIT;
\elif :rnd <= 70
-- Hot update (20%): Zipfian-distributed update
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :hot_aid;
COMMIT;
\elif :rnd <= 85
-- Cold read (15%): uniform random point lookup (may cause cache miss)
SELECT abalance FROM pgbench_accounts WHERE aid = :cold_aid;
COMMIT;
\elif :rnd <= 95
-- Cold update (10%): uniform random update
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :cold_aid;
COMMIT;
\else
-- Rollback (5%): Zipfian hot row update + rollback
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :hot_aid;
ROLLBACK;
\endif
