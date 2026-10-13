--
-- Role: Scanner; range scans across moderate windows
--
-- Used by the multi-role concurrent benchmark (W9).
-- Exercises sequential I/O, UNDO visibility across many rows,
-- and interaction with concurrent updates.
--
\set start random(1, 100000 * :scale - 10000)
SELECT count(*), avg(abalance) FROM pgbench_accounts
WHERE aid BETWEEN :start AND :start + 9999;
