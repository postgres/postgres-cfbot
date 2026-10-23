--
-- pgbench-b (Select-Only query)
--
-- Executes the standard single-statement Select-Only query against pgbench_accounts.
--

\set aid random(1, 100000 * :scale)

SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
