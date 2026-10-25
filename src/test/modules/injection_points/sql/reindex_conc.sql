-- Tests for REINDEX CONCURRENTLY
CREATE EXTENSION injection_points;

-- Check safety of indexes with predicates and expressions.
SELECT injection_points_set_local();
SELECT injection_points_attach('reindex-conc-index-safe', 'notice');
SELECT injection_points_attach('reindex-conc-index-not-safe', 'notice');

CREATE SCHEMA reindex_inj;
CREATE TABLE reindex_inj.tbl(i int primary key, updated_at timestamp);

CREATE UNIQUE INDEX ind_simple ON reindex_inj.tbl(i);
CREATE UNIQUE INDEX ind_expr ON reindex_inj.tbl(ABS(i));
CREATE UNIQUE INDEX ind_pred ON reindex_inj.tbl(i) WHERE mod(i, 2) = 0;
CREATE UNIQUE INDEX ind_expr_pred ON reindex_inj.tbl(abs(i)) WHERE mod(i, 2) = 0;

REINDEX INDEX CONCURRENTLY reindex_inj.ind_simple;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_expr;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_pred;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_expr_pred;

-- Validation must reset snapshots even when STIR is empty.
SELECT injection_points_attach('validate-index-snapshot-reset', 'notice');
SELECT injection_points_attach('validate-index-xmin-not-advanced', 'notice');
SET concurrent_index_reset_snapshot_interval = 0;
INSERT INTO reindex_inj.tbl(i) VALUES (1), (2), (3);
CREATE INDEX CONCURRENTLY ind_reset ON reindex_inj.tbl(i);
REINDEX INDEX CONCURRENTLY reindex_inj.ind_reset;

-- These cases keep a fixed snapshot.
SET concurrent_index_reset_snapshot_interval = -1;
REINDEX INDEX CONCURRENTLY reindex_inj.ind_reset;
SET concurrent_index_reset_snapshot_interval = 0;
SET default_transaction_isolation = 'repeatable read';
REINDEX INDEX CONCURRENTLY reindex_inj.ind_reset;
SET default_transaction_isolation = 'serializable';
REINDEX INDEX CONCURRENTLY reindex_inj.ind_reset;
RESET default_transaction_isolation;
RESET concurrent_index_reset_snapshot_interval;

-- Cleanup
SELECT injection_points_detach('validate-index-snapshot-reset');
SELECT injection_points_detach('validate-index-xmin-not-advanced');
SELECT injection_points_detach('reindex-conc-index-safe');
SELECT injection_points_detach('reindex-conc-index-not-safe');
DROP TABLE reindex_inj.tbl;
DROP SCHEMA reindex_inj;

DROP EXTENSION injection_points;
