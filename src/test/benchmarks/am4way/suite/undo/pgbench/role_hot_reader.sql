--
-- Role: Hot reader; fast point lookups on frequently-accessed rows
--
-- Used by the multi-role concurrent benchmark (W9).
-- Exercises buffer cache hits and UNDO visibility checks.
--
\set aid random(1, 1000)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
