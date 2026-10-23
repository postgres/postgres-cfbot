--
-- pgbench-b (modernized, database-function based transaction)
--
-- Executes the classic 5-statement banking transaction inside a single
-- stored procedure call, eliminating client-server roundtrips and
-- running cleanly under PostgreSQL autocommit mode.
--

\set aid random(1, 100000 * :scale)
\set bid random(1, :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)

CALL pgbench_b_transaction(:aid, :bid, :tid, :delta);
