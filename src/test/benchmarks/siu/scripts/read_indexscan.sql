-- read_indexscan: read-only btree index-scan workload confirming the
-- HOT-indexed read path adds no per-scan overhead on tables with no stale
-- entries.
-- The crossed-attribute bitmap decides staleness without a key comparison and
-- the scan does not request the index tuple, so on a freshly reset siu_table
-- (no stale HOT-indexed entries) there should be no master-vs-tepid
-- difference on this cell.  The predicate is an equality on the
-- indexed column b and the target list includes the non-indexed column e,
-- forcing a plain (heap-fetching) index scan rather than an index-only scan.
\set id random(1, :rows)
SELECT a, b, c, d, e FROM siu_table WHERE b = :id;
