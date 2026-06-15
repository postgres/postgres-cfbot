-- Wide-table workload.  The setup script creates a table with WIDE_COLS integer
-- columns, each separately btree-indexed.  The workload UPDATEs a
-- configurable number of those indexed columns per transaction
-- (driven by WIDE_STEPS via run.sh) on a random row.
\set rid random(1, :scale * 1000)
\set v random(1, 1000000000)
UPDATE wide_table SET :wide_set_clause WHERE id = :rid;
