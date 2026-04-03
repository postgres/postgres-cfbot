CREATE EXTENSION injection_points;

SELECT injection_points_set_local();
SELECT injection_points_attach('heap_reset_scan_snapshot_effective', 'notice');
SELECT injection_points_attach('table_beginscan_strat_reset_snapshots', 'notice');


CREATE SCHEMA cic_reset_snap;
CREATE TABLE cic_reset_snap.tbl(i int primary key, j int);
INSERT INTO cic_reset_snap.tbl SELECT i, i * I FROM generate_series(1, 200) s(i);

CREATE FUNCTION cic_reset_snap.predicate_stable(integer) RETURNS bool IMMUTABLE
									  LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE 'SELECT txid_current()';
    RETURN MOD($1, 2) = 0;
END; $$;

CREATE FUNCTION cic_reset_snap.predicate_stable_no_param() RETURNS bool IMMUTABLE
									  LANGUAGE plpgsql AS $$
BEGIN
    EXECUTE 'SELECT txid_current()';
    RETURN false;
END; $$;

----------------
ALTER TABLE cic_reset_snap.tbl SET (parallel_workers=0);

CREATE UNIQUE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(MOD(i, 2), j) WHERE MOD(i, 2) = 0;
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable_no_param();
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING BRIN(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

-- The same in parallel mode
ALTER TABLE cic_reset_snap.tbl SET (parallel_workers=2);

CREATE UNIQUE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(MOD(i, 2), j) WHERE MOD(i, 2) = 0;
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i, j) WHERE cic_reset_snap.predicate_stable_no_param();
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i DESC NULLS LAST);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl USING BRIN(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

BEGIN TRANSACTION;
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
ROLLBACK ;

SET default_transaction_isolation = 'repeatable read';
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;

SET default_transaction_isolation = serializable;
CREATE INDEX CONCURRENTLY idx ON cic_reset_snap.tbl(i);
REINDEX INDEX CONCURRENTLY cic_reset_snap.idx;
DROP INDEX CONCURRENTLY cic_reset_snap.idx;
RESET default_transaction_isolation;

DROP SCHEMA cic_reset_snap CASCADE;

DROP EXTENSION injection_points;
