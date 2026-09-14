--
-- Role: Updater; uniform random updates with occasional rollback
--
-- Used by the multi-role concurrent benchmark (W9).
-- 80% commit, 20% rollback; exercises UNDO write and rollback paths.
--
\set rnd random(1, 100)
\set aid random(1, 100000 * :scale)
\set delta random(-5000, 5000)

BEGIN;
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
\if :rnd <= 80
COMMIT;
\else
ROLLBACK;
\endif
