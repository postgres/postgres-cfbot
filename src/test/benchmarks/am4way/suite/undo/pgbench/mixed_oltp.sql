--
-- Mixed OLTP pgbench script: 40% SELECT, 30% UPDATE, 20% INSERT, 10% ROLLBACK
--
-- The 10% rollback rate exercises UNDO's synchronous rollback path in a
-- realistic OLTP mix. Use with: pgbench -f mixed_oltp.sql
--

\set rnd random(1, 100)
\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)

BEGIN;

\if :rnd <= 40
-- SELECT (40%)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
COMMIT;
\elif :rnd <= 70
-- UPDATE (30%)
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
COMMIT;
\elif :rnd <= 90
-- INSERT (20%)
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime)
VALUES (:tid, :bid, :aid, :delta, CURRENT_TIMESTAMP);
COMMIT;
\else
-- ROLLBACK (10%) - exercises UNDO synchronous rollback
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
ROLLBACK;
\endif
