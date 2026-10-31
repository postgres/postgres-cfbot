--
-- Role: Cold reader; uniform random reads across the full table
--
-- Used by the multi-role concurrent benchmark (W9).
-- Exercises buffer cache misses and I/O-bound read paths.
--
\set aid random(1, 100000 * :scale)
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
