SELECT current_setting('max_prepared_transactions')::integer < 2 AS skip_test \gset

-- ACL coverage: pg_xmin_horizon is REVOKE'd from PUBLIC and GRANT'd to
-- pg_read_all_stats.
CREATE ROLE regress_pxh_unpriv;
CREATE ROLE regress_pxh_priv;
GRANT pg_read_all_stats TO regress_pxh_priv;

SET ROLE regress_pxh_unpriv;
SELECT count(*) FROM pg_xmin_horizon;
RESET ROLE;

SET ROLE regress_pxh_priv;
SELECT count(*) >= 0 AS readable FROM pg_xmin_horizon;
RESET ROLE;

DROP ROLE regress_pxh_unpriv, regress_pxh_priv;

\if :skip_test
\quit
\endif
--
-- XMIN_HORIZON
--

-- Structure probe.
SELECT * FROM pg_xmin_horizon WHERE false;

-- A REPEATABLE READ backend with a snapshot but no xid contributes a row with
-- all three classes equal to its backend_xmin, and xact_start joined from
-- pg_stat_activity.
BEGIN TRANSACTION ISOLATION LEVEL REPEATABLE READ;
SELECT 1;

SELECT
    data_xmin = catalog_xmin AND catalog_xmin = shared_xmin AS classes_equal,
    data_xmin = (SELECT backend_xmin
                 FROM pg_stat_activity
                 WHERE pid = pg_backend_pid()) AS matches_backend_xmin,
    xact_start = (SELECT xact_start
                  FROM pg_stat_activity
                  WHERE pid = pg_backend_pid()) AS xact_start_correct
FROM pg_xmin_horizon
WHERE kind = 'backend'
  AND pid = pg_backend_pid();

COMMIT;

-- Writing transaction with assigned xid: data_xmin = backend_xmin still holds.
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT pg_current_xact_id() IS NOT NULL AS xid_assigned;

SELECT
    data_xmin = catalog_xmin AND catalog_xmin = shared_xmin AS classes_equal,
    data_xmin = (SELECT backend_xmin
                 FROM pg_stat_activity
                 WHERE pid = pg_backend_pid()) AS matches_backend_xmin
FROM pg_xmin_horizon
WHERE kind = 'backend'
  AND pid = pg_backend_pid();

COMMIT;

-- A prepared transaction's dummy PGPROC must be reported as kind =
-- 'prepared_xact' with a non-null data_xmin, and it must not leak as
-- a backend row with pid = 0.
CREATE TABLE xmin_horizon_t(i int);
BEGIN;
INSERT INTO xmin_horizon_t VALUES (1);
PREPARE TRANSACTION 'xmin_horizon_p';

SELECT count(*) AS prepared_xact_rows
FROM pg_xmin_horizon
WHERE kind = 'prepared_xact' AND gid = 'xmin_horizon_p'
  AND data_xmin IS NOT NULL;

SELECT count(*) AS leaked_backend_rows
FROM pg_xmin_horizon
WHERE kind = 'backend' AND pid = 0;

-- gid joined in from pg_prepared_xacts and xact_start from
-- pg_prepared_xacts.prepared.
SELECT gid = 'xmin_horizon_p' AS gid_matches,
       xact_start = (SELECT prepared FROM pg_prepared_xacts
                     WHERE gid = 'xmin_horizon_p') AS xact_start_matches_prepared
FROM pg_xmin_horizon
WHERE kind = 'prepared_xact' AND gid = 'xmin_horizon_p';

ROLLBACK PREPARED 'xmin_horizon_p';
DROP TABLE xmin_horizon_t;
