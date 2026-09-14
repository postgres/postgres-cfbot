-- A one-page heap twin makes physical page counts comparable despite headers.
CREATE TABLE fvs_heap (a int PRIMARY KEY) USING heap WITH (autovacuum_enabled=false);
CREATE TABLE fvs_flux (a int PRIMARY KEY) USING flux WITH (autovacuum_enabled=false);
INSERT INTO fvs_heap SELECT generate_series(1,20);
INSERT INTO fvs_flux SELECT generate_series(1,20);
VACUUM fvs_heap;
VACUUM fvs_flux;
SELECT relname, relpages = 1 AS pages, reltuples = 20 AS tuples,
       relallvisible = 1 AS visible
FROM pg_class WHERE relname IN ('fvs_heap', 'fvs_flux') ORDER BY relname;
SELECT pg_stat_force_next_flush();
SELECT pg_stat_clear_snapshot();
SELECT relname, vacuum_count = 1 AS vacuum_reported
FROM pg_stat_user_tables WHERE relname IN ('fvs_heap', 'fvs_flux') ORDER BY relname;
-- Burn an xid so a no-op freeze cannot pass on a newly created relation.
SELECT pg_current_xact_id() IS NOT NULL AS assigned;
VACUUM (FREEZE) fvs_heap;
VACUUM (FREEZE) fvs_flux;
SELECT relname, age(relfrozenxid) <= 1 AS frozen,
       relallfrozen = 1 AS frozen_page
FROM pg_class WHERE relname IN ('fvs_heap', 'fvs_flux') ORDER BY relname;
-- A repeated vacuum must count pages skipped as all-frozen, not report zero.
VACUUM fvs_heap;
VACUUM fvs_flux;
SELECT relname, reltuples = 20 AS tuples
FROM pg_class WHERE relname IN ('fvs_heap', 'fvs_flux') ORDER BY relname;
-- Force bulk-delete so both index AMs return accurate cleanup statistics.
INSERT INTO fvs_heap VALUES (99);
INSERT INTO fvs_flux VALUES (99);
DELETE FROM fvs_heap WHERE a=99;
DELETE FROM fvs_flux WHERE a=99;
VACUUM (INDEX_CLEANUP ON) fvs_heap;
VACUUM (INDEX_CLEANUP ON) fvs_flux;
SELECT relname, relpages > 0 AS pages, reltuples = 20 AS tuples
FROM pg_class WHERE relname IN ('fvs_heap_pkey', 'fvs_flux_pkey') ORDER BY relname;
-- A cursor stores a version identity before DML freezes its before-image.
BEGIN;
DECLARE fvs_h CURSOR FOR SELECT * FROM fvs_heap WHERE a=1;
DECLARE fvs_f CURSOR FOR SELECT * FROM fvs_flux WHERE a=1;
FETCH fvs_h;
FETCH fvs_f;
UPDATE fvs_heap SET a=101 WHERE CURRENT OF fvs_h;
UPDATE fvs_flux SET a=101 WHERE CURRENT OF fvs_f;
ROLLBACK;
DROP TABLE fvs_heap, fvs_flux;
