-- Tests for how a heap rewrite treats an invalid index
CREATE EXTENSION injection_points;

SELECT injection_points_set_local();

-- A CREATE INDEX CONCURRENTLY that fails after the index became ready for
-- inserts leaves it invalid, but maintained by DML, so a rewrite has to
-- rebuild it.
SELECT injection_points_attach('define-index-before-set-valid', 'error');
CREATE TABLE index_inj_tbl (i int);
INSERT INTO index_inj_tbl VALUES (1), (2);
CREATE INDEX CONCURRENTLY index_inj_idx ON index_inj_tbl (i);
SELECT injection_points_detach('define-index-before-set-valid');
SELECT indisvalid, indisready FROM pg_index
WHERE indexrelid = 'index_inj_idx'::regclass;
SELECT relfilenode AS inj_idx_node FROM pg_class
WHERE oid = 'index_inj_idx'::regclass \gset
VACUUM FULL index_inj_tbl;
SELECT relfilenode = :inj_idx_node FROM pg_class
WHERE oid = 'index_inj_idx'::regclass;

-- Cleanup
DROP TABLE index_inj_tbl;

DROP EXTENSION injection_points;
