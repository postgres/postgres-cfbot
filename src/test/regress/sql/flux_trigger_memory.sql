--
-- Memory held for AFTER ROW trigger events across a long transaction.
--
-- A FLUX row event carries copies of its row images (the TID names only the
-- newest version).  Those copies must go away with the event: an immediate
-- event is gone at the end of its statement, an event of an aborted
-- subtransaction is gone at its rollback.  Each case runs many top-level
-- statements in one transaction and compares the after-trigger event memory
-- after the first statement with that after the last; heap is the control.
--
CREATE FUNCTION tm_ctx_bytes() RETURNS bigint LANGUAGE sql AS $$
  SELECT coalesce(sum(total_bytes), 0)::bigint FROM pg_backend_memory_contexts
  WHERE name LIKE 'AfterTrigger%'
$$;
CREATE TEMP TABLE tm_mem (k text, n int, bytes bigint);
CREATE FUNCTION tm_noop() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RETURN NULL;
END $$;

-- Immediate AFTER UPDATE row trigger
CREATE TABLE tm_trig_heap (id int PRIMARY KEY, v int) USING heap;
CREATE TABLE tm_trig_flux (id int PRIMARY KEY, v int) USING flux;
INSERT INTO tm_trig_heap SELECT g, 0 FROM generate_series(1, 1000) g;
INSERT INTO tm_trig_flux SELECT g, 0 FROM generate_series(1, 1000) g;
CREATE TRIGGER tm_after AFTER UPDATE ON tm_trig_heap
  FOR EACH ROW EXECUTE FUNCTION tm_noop();
CREATE TRIGGER tm_after AFTER UPDATE ON tm_trig_flux
  FOR EACH ROW EXECUTE FUNCTION tm_noop();

-- Immediate foreign key check (RI trigger on the referencing table)
CREATE TABLE tm_pk_heap (id int PRIMARY KEY) USING heap;
CREATE TABLE tm_pk_flux (id int PRIMARY KEY) USING flux;
INSERT INTO tm_pk_heap VALUES (1), (2);
INSERT INTO tm_pk_flux VALUES (1), (2);
CREATE TABLE tm_fk_heap (id int PRIMARY KEY, pid int REFERENCES tm_pk_heap)
  USING heap;
CREATE TABLE tm_fk_flux (id int PRIMARY KEY, pid int REFERENCES tm_pk_flux)
  USING flux;
INSERT INTO tm_fk_heap SELECT g, 1 FROM generate_series(1, 1000) g;
INSERT INTO tm_fk_flux SELECT g, 1 FROM generate_series(1, 1000) g;

-- Each case: one transaction of 200 top-level UPDATEs of 1000 rows (each in a
-- rolled-back savepoint for the subxact cases), recording the event memory
-- after the first and after the last.
\set ECHO none
\o /dev/null
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$UPDATE tm_trig_heap SET v = v + 1; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'trig_heap') AS s
  UNION ALL SELECT 2, $q$UPDATE tm_trig_heap SET v = v + 1$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$UPDATE tm_trig_heap SET v = v + 1; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'trig_heap')
) s ORDER BY o \gexec
COMMIT;
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$UPDATE tm_trig_flux SET v = v + 1; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'trig_flux') AS s
  UNION ALL SELECT 2, $q$UPDATE tm_trig_flux SET v = v + 1$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$UPDATE tm_trig_flux SET v = v + 1; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'trig_flux')
) s ORDER BY o \gexec
COMMIT;
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$UPDATE tm_fk_heap SET pid = 3 - pid; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'fk_heap') AS s
  UNION ALL SELECT 2, $q$UPDATE tm_fk_heap SET pid = 3 - pid$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$UPDATE tm_fk_heap SET pid = 3 - pid; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'fk_heap')
) s ORDER BY o \gexec
COMMIT;
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$UPDATE tm_fk_flux SET pid = 3 - pid; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'fk_flux') AS s
  UNION ALL SELECT 2, $q$UPDATE tm_fk_flux SET pid = 3 - pid$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$UPDATE tm_fk_flux SET pid = 3 - pid; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'fk_flux')
) s ORDER BY o \gexec
COMMIT;
\o
\set ECHO all

-- Deferred trigger events of rolled-back subtransactions
CREATE CONSTRAINT TRIGGER tm_deferred AFTER UPDATE ON tm_trig_heap
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION tm_noop();
CREATE CONSTRAINT TRIGGER tm_deferred AFTER UPDATE ON tm_trig_flux
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION tm_noop();
\set ECHO none
\o /dev/null
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$SAVEPOINT s; UPDATE tm_trig_heap SET v = v + 1; ROLLBACK TO s; RELEASE s; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'subxact_heap') AS s
  UNION ALL SELECT 2, $q$SAVEPOINT s; UPDATE tm_trig_heap SET v = v + 1; ROLLBACK TO s; RELEASE s$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$SAVEPOINT s; UPDATE tm_trig_heap SET v = v + 1; ROLLBACK TO s; RELEASE s; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'subxact_heap')
) s ORDER BY o \gexec
COMMIT;
BEGIN;
SELECT s FROM (
  SELECT 1 AS o, format($q$SAVEPOINT s; UPDATE tm_trig_flux SET v = v + 1; ROLLBACK TO s; RELEASE s; INSERT INTO tm_mem SELECT %L, 1, tm_ctx_bytes()$q$, 'subxact_flux') AS s
  UNION ALL SELECT 2, $q$SAVEPOINT s; UPDATE tm_trig_flux SET v = v + 1; ROLLBACK TO s; RELEASE s$q$ FROM generate_series(2, 199)
  UNION ALL SELECT 3, format($q$SAVEPOINT s; UPDATE tm_trig_flux SET v = v + 1; ROLLBACK TO s; RELEASE s; INSERT INTO tm_mem SELECT %L, 200, tm_ctx_bytes()$q$, 'subxact_flux')
) s ORDER BY o \gexec
COMMIT;
\o
\set ECHO all

-- One statement's FLUX images take about 250 kB, so keeping them for the
-- transaction grows the context by ~50 MB here.  Heap grows by well under
-- 1 MB (per-statement bookkeeping, e.g. the modified-columns bitmaps).
SELECT a.k, b.bytes - a.bytes < 4 * 1024 * 1024 AS flat
FROM tm_mem a JOIN tm_mem b ON a.k = b.k AND a.n = 1 AND b.n > 1
ORDER BY a.k;

-- The deferred events of a committed statement still fire, with their images
CREATE TABLE tm_log (tab text, o int, n int);
CREATE FUNCTION tm_log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO tm_log VALUES (TG_TABLE_NAME, OLD.v, NEW.v);
  RETURN NULL;
END $$;
UPDATE tm_trig_heap SET v = 0;
UPDATE tm_trig_flux SET v = 0;
CREATE CONSTRAINT TRIGGER tm_deferred_log AFTER UPDATE ON tm_trig_heap
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION tm_log_row();
CREATE CONSTRAINT TRIGGER tm_deferred_log AFTER UPDATE ON tm_trig_flux
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION tm_log_row();
BEGIN;
UPDATE tm_trig_heap SET v = 1000 WHERE id <= 3;
UPDATE tm_trig_flux SET v = 1000 WHERE id <= 3;
SAVEPOINT s1;
UPDATE tm_trig_heap SET v = 2000 WHERE id <= 3;
UPDATE tm_trig_flux SET v = 2000 WHERE id <= 3;
ROLLBACK TO s1;
SAVEPOINT s2;
UPDATE tm_trig_heap SET v = v + 1 WHERE id <= 3;
UPDATE tm_trig_flux SET v = v + 1 WHERE id <= 3;
RELEASE s2;
UPDATE tm_trig_heap SET v = v + 1 WHERE id <= 2;
UPDATE tm_trig_flux SET v = v + 1 WHERE id <= 2;
COMMIT;
SELECT tab, o, n, count(*) FROM tm_log GROUP BY 1, 2, 3 ORDER BY 1, 2, 3;
SELECT h.id, h.v AS heap_v, f.v AS flux_v
FROM tm_trig_heap h JOIN tm_trig_flux f USING (id) WHERE id <= 3 ORDER BY id;

DROP TABLE tm_trig_heap, tm_trig_flux, tm_fk_heap, tm_fk_flux, tm_pk_heap,
  tm_pk_flux, tm_log;
DROP FUNCTION tm_ctx_bytes(), tm_noop(), tm_log_row();
