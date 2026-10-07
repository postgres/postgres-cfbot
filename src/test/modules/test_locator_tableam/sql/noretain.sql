--
-- A table AM whose old row versions are not fetchable by locator after an
-- UPDATE.  Everything that reports the pre-update row must have captured it
-- before the write.
--
CREATE EXTENSION test_locator_tableam;

CREATE TABLE t (id int PRIMARY KEY, v int, pad text) USING heap_noretain;
INSERT INTO t SELECT g, g * 10, 'x' FROM generate_series(1, 3) g;

-- RETURNING OLD, for UPDATE, DELETE and MERGE
UPDATE t SET v = v + 1 WHERE id = 1 RETURNING old.v AS old_v, new.v AS new_v;
DELETE FROM t WHERE id = 1 RETURNING old.v AS old_v, new.v AS new_v;
INSERT INTO t VALUES (1, 10, 'x');
MERGE INTO t USING (VALUES (1), (2)) s(id) ON t.id = s.id
  WHEN MATCHED AND t.id = 1 THEN UPDATE SET v = t.v + 5
  WHEN MATCHED THEN DELETE
  RETURNING merge_action(), old.v AS old_v, new.v AS new_v;
INSERT INTO t VALUES (2, 20, 'x');
INSERT INTO t VALUES (1, 0, 'x') ON CONFLICT (id) DO UPDATE SET v = t.v + 1
  RETURNING old.v AS old_v, new.v AS new_v;
UPDATE t SET v = 10 WHERE id = 1;

-- AFTER ROW triggers see the old row, immediate and deferred
CREATE TABLE log (op text, old_v int, new_v int);
CREATE FUNCTION log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO log VALUES (TG_OP || ' ' || TG_NAME, OLD.v, NEW.v);
  RETURN NULL;
END $$;
CREATE TRIGGER t_after AFTER UPDATE OR DELETE ON t
  FOR EACH ROW EXECUTE FUNCTION log_row();
CREATE CONSTRAINT TRIGGER t_deferred AFTER UPDATE OR DELETE ON t
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION log_row();
BEGIN;
UPDATE t SET v = v + 1 WHERE id = 1;
UPDATE t SET v = v + 1 WHERE id = 1;
DELETE FROM t WHERE id = 3;
SELECT * FROM log ORDER BY op, old_v;
COMMIT;
-- The deferred UPDATE triggers see each event's own OLD and NEW, as on heap.
SELECT * FROM log ORDER BY op, old_v;
DROP TRIGGER t_after ON t;
DROP TRIGGER t_deferred ON t;
TRUNCATE log;

-- MERGE through the after-trigger queue
CREATE TRIGGER t_after AFTER UPDATE OR DELETE ON t
  FOR EACH ROW EXECUTE FUNCTION log_row();
MERGE INTO t USING (VALUES (1), (2)) s(id) ON t.id = s.id
  WHEN MATCHED AND t.id = 1 THEN UPDATE SET v = 100
  WHEN MATCHED THEN DELETE;
SELECT * FROM log ORDER BY op, old_v;
DROP TRIGGER t_after ON t;
TRUNCATE log;
TRUNCATE t;
INSERT INTO t SELECT g, g * 10, 'x' FROM generate_series(1, 3) g;

-- Transition tables
CREATE FUNCTION log_tables() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF TG_OP = 'UPDATE' THEN
    INSERT INTO log SELECT 'old ' || TG_OP, o.v, NULL FROM old_rows o;
    INSERT INTO log SELECT 'new ' || TG_OP, NULL, n.v FROM new_rows n;
  ELSE
    INSERT INTO log SELECT 'old ' || TG_OP, o.v, NULL FROM old_rows o;
  END IF;
  RETURN NULL;
END $$;
CREATE TRIGGER t_upd_tables AFTER UPDATE ON t
  REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
  FOR EACH STATEMENT EXECUTE FUNCTION log_tables();
CREATE TRIGGER t_del_tables AFTER DELETE ON t
  REFERENCING OLD TABLE AS old_rows
  FOR EACH STATEMENT EXECUTE FUNCTION log_tables();
UPDATE t SET v = v + 1;
DELETE FROM t WHERE id = 3;
SELECT * FROM log ORDER BY op, old_v, new_v;
DROP TRIGGER t_upd_tables ON t;
DROP TRIGGER t_del_tables ON t;
TRUNCATE log;

-- Foreign keys on a parent that does not retain old versions
CREATE TABLE p (id int PRIMARY KEY) USING heap_noretain;
CREATE TABLE c_cascade (ref int REFERENCES p ON UPDATE CASCADE);
CREATE TABLE c_restrict (ref int REFERENCES p ON UPDATE RESTRICT);
INSERT INTO p VALUES (1), (2), (3);
INSERT INTO c_cascade VALUES (1);
INSERT INTO c_restrict VALUES (3);
UPDATE p SET id = 10 WHERE id = 1;
SELECT * FROM c_cascade;
UPDATE p SET id = 30 WHERE id = 3;
UPDATE p SET id = 20 WHERE id = 2;
SELECT * FROM p ORDER BY id;
DROP TABLE c_cascade, c_restrict;

-- A deferred foreign key check on a child that does not retain old versions.
-- The INSERT's check must look at the row as it is at COMMIT.
CREATE TABLE c_deferred (id int PRIMARY KEY, ref int
  REFERENCES p DEFERRABLE INITIALLY DEFERRED) USING heap_noretain;
BEGIN;
INSERT INTO c_deferred VALUES (1, 99);
UPDATE c_deferred SET ref = 10 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE c_deferred SET ref = 99 WHERE id = 1;
UPDATE c_deferred SET ref = 20 WHERE id = 1;
COMMIT;
SELECT * FROM c_deferred;
BEGIN;
UPDATE c_deferred SET ref = 99 WHERE id = 1;
COMMIT;
-- The current version is the one the rolled back savepoint did not replace.
BEGIN;
UPDATE c_deferred SET ref = 99 WHERE id = 1;
SAVEPOINT s;
UPDATE c_deferred SET ref = 20 WHERE id = 1;
ROLLBACK TO SAVEPOINT s;
COMMIT;
DROP TABLE c_deferred, p;

-- A deferred unique constraint
CREATE TABLE u (id int UNIQUE DEFERRABLE INITIALLY DEFERRED) USING heap_noretain;
INSERT INTO u VALUES (1), (2);
BEGIN;
UPDATE u SET id = 3 - id;
COMMIT;
BEGIN;
UPDATE u SET id = 1;
COMMIT;
SELECT * FROM u ORDER BY id;
DROP TABLE u;

-- Old rows spill to a temporary file beyond work_mem, and a rolled back
-- subtransaction discards the ones it queued.
CREATE TABLE big (id int PRIMARY KEY, v int, pad text) USING heap_noretain;
INSERT INTO big SELECT g, g, repeat('x', 100) FROM generate_series(1, 2000) g;
CREATE TABLE big_log (id int, old_v int, new_v int);
CREATE FUNCTION log_big() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO big_log VALUES (OLD.id, OLD.v, NEW.v);
  RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER big_deferred AFTER UPDATE ON big
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION log_big();
SET work_mem = '64kB';
SET temp_tablespaces = '';
BEGIN;
UPDATE big SET v = v + 10000 WHERE id <= 1000;
SAVEPOINT s;
UPDATE big SET v = v + 20000;
ROLLBACK TO SAVEPOINT s;
UPDATE big SET v = v + 30000 WHERE id > 1000;
SELECT count(*) > 0 AS spilled FROM pg_ls_tmpdir();
COMMIT;
RESET temp_tablespaces;
RESET work_mem;
SELECT count(*), sum(old_v), sum(new_v) FROM big_log;
SELECT count(*) FROM big_log WHERE (id <= 1000 AND old_v <> id) OR
  (id > 1000 AND old_v <> id) OR new_v - old_v NOT IN (10000, 30000);

-- A failed write to the spill file aborts the transaction cleanly.
SET work_mem = '64kB';
SET temp_file_limit = '100kB';
\set VERBOSITY terse
UPDATE big SET v = v + 1;
\set VERBOSITY default
SELECT 1;
-- A spill file first created in a failed subtransaction is abandoned with it.
BEGIN;
UPDATE big SET v = v + 1 WHERE id <= 100;
SAVEPOINT s;
\set VERBOSITY terse
UPDATE big SET v = v + 1;
\set VERBOSITY default
ROLLBACK TO SAVEPOINT s;
UPDATE big SET v = v + 1 WHERE id <= 10;
COMMIT;
-- A spill file that predates a failed subtransaction is kept, without the
-- failed write, so the transaction can still commit and fire its deferred
-- triggers from it.
RESET temp_file_limit;
SET temp_tablespaces = '';
BEGIN;
UPDATE big SET v = v + 1;
SELECT ceil(sum(size) / 1024.0)::int + 40 AS limit_kb FROM pg_ls_tmpdir() \gset
SET temp_file_limit = :'limit_kb';
SAVEPOINT s;
DO $$
BEGIN
  UPDATE big SET v = v + 1;
EXCEPTION WHEN configuration_limit_exceeded THEN
  RAISE NOTICE 'spill failed';
END $$;
ROLLBACK TO SAVEPOINT s;
UPDATE big SET v = v + 1 WHERE id <= 10;
COMMIT;
RESET temp_tablespaces;
RESET temp_file_limit;
-- Images are released at the end of each top-level statement once no
-- deferred event can refer to them.
DROP TRIGGER big_deferred ON big;
CREATE TRIGGER big_after AFTER UPDATE ON big
  FOR EACH ROW EXECUTE FUNCTION log_big();
SET temp_tablespaces = '';
BEGIN;
UPDATE big SET v = v + 1;
SELECT coalesce(sum(size), 0) AS tmp1 FROM pg_ls_tmpdir() \gset
UPDATE big SET v = v + 1;
SELECT coalesce(sum(size), 0) <= :tmp1 AS released FROM pg_ls_tmpdir();
COMMIT;
RESET temp_tablespaces;
RESET work_mem;
DROP TABLE big, big_log;

-- A row trigger that changes another row of the same statement.  Each event
-- carries its own OLD and NEW, as on heap.
CREATE TABLE im (id int PRIMARY KEY, v int) USING heap_noretain;
INSERT INTO im VALUES (1, 10), (2, 20);
CREATE TABLE im_log (id int, old_v int, new_v int);
CREATE FUNCTION im_trig() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO im_log VALUES (NEW.id, OLD.v, NEW.v);
  IF NEW.id = 1 THEN
    UPDATE im SET v = 100 WHERE id = 2;
  END IF;
  RETURN NULL;
END $$;
CREATE TRIGGER im_after AFTER UPDATE ON im
  FOR EACH ROW EXECUTE FUNCTION im_trig();
UPDATE im SET v = v + 1;
SELECT * FROM im_log WHERE id = 2 ORDER BY old_v, new_v;
DROP TABLE im, im_log;
DROP FUNCTION im_trig();

-- Cross-partition UPDATE out of a partition that does not retain old versions
CREATE TABLE pt (id int, v int, PRIMARY KEY (id)) PARTITION BY RANGE (id);
CREATE TABLE pt1 PARTITION OF pt FOR VALUES FROM (0) TO (100) USING heap_noretain;
CREATE TABLE pt2 PARTITION OF pt FOR VALUES FROM (100) TO (200) USING heap_noretain;
CREATE TABLE pt_ref (ref int REFERENCES pt ON UPDATE CASCADE);
INSERT INTO pt VALUES (1, 10), (2, 20);
INSERT INTO pt_ref VALUES (1);
CREATE TRIGGER pt_tables AFTER UPDATE ON pt
  REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
  FOR EACH STATEMENT EXECUTE FUNCTION log_tables();
CREATE TRIGGER pt_after AFTER UPDATE OR DELETE ON pt
  FOR EACH ROW EXECUTE FUNCTION log_row();
WITH u AS (
  UPDATE pt SET id = id + 100, v = v + 1
    RETURNING old.id AS old_id, old.v AS old_v, new.id AS new_id, new.v AS new_v)
SELECT * FROM u ORDER BY old_id;
SELECT * FROM log ORDER BY op, old_v, new_v;
SELECT * FROM pt_ref;
DROP TABLE pt_ref, pt, log, t;
DROP FUNCTION log_row(), log_tables(), log_big();
