--
-- pgbench-b Post-Load Constraints, Indexes, and Vacuum Analyze
--

ALTER TABLE pgbench_branches ADD PRIMARY KEY (bid);
ALTER TABLE pgbench_tellers ADD PRIMARY KEY (tid);
ALTER TABLE pgbench_accounts ADD PRIMARY KEY (aid);

ALTER TABLE pgbench_tellers ADD CONSTRAINT pgbench_tellers_bid_fkey
    FOREIGN KEY (bid) REFERENCES pgbench_branches(bid);
ALTER TABLE pgbench_accounts ADD CONSTRAINT pgbench_accounts_bid_fkey
    FOREIGN KEY (bid) REFERENCES pgbench_branches(bid);

VACUUM ANALYZE pgbench_branches;
VACUUM ANALYZE pgbench_tellers;
VACUUM ANALYZE pgbench_accounts;
VACUUM ANALYZE pgbench_history;
