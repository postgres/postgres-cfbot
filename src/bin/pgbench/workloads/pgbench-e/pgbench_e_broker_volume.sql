--
-- pgbench-e (TPC-E-inspired) Broker-Volume Transaction Script
--

\set b_min random(1, 5 * :scale)
\set b_max (:b_min + 5)

CALL pgbench_e_broker_volume(:b_min, :b_max);
