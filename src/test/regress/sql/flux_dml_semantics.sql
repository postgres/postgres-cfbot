-- Run the same statements against heap and FLUX.
CREATE SCHEMA semantics_heap;
CREATE SCHEMA semantics_flux;
SET search_path = semantics_heap, public;
CREATE TABLE m (tid int PRIMARY KEY, bal int) USING heap;
INSERT INTO m VALUES (1, 10);
MERGE INTO m USING (VALUES (1),(1)) s(sid) ON m.tid = s.sid
  WHEN MATCHED THEN UPDATE SET bal = 0;
INSERT INTO m VALUES (1, 1), (1, 2)
  ON CONFLICT (tid) DO UPDATE SET bal = 99;
TRUNCATE m;
INSERT INTO m VALUES (1, 1), (1, 2)
  ON CONFLICT (tid) DO UPDATE SET bal = 99;
CREATE TABLE cc (a int) USING heap;
BEGIN;
INSERT INTO cc VALUES (1);
INSERT INTO cc VALUES (2);
SELECT a, cmin, cmax, xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax FROM cc ORDER BY a;
UPDATE cc SET a = a + 10;
SELECT a, cmin, cmax, xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax FROM cc ORDER BY a;
-- FLUX has no heap combo-CID word: old undo versions retain their
-- inserting CID, not heap's raw combo index. Test the live versions here.
INSERT INTO cc VALUES (3) RETURNING a, cmin, cmax;
UPDATE cc SET a = a + 10 WHERE a = 3 RETURNING a, cmin, cmax;
COMMIT;
CREATE FUNCTION check_oid() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE '%: old tableoid valid %, new tableoid valid %',
    TG_NAME, OLD.tableoid = TG_RELID, NEW.tableoid = TG_RELID;
  RETURN NEW;
END
$$;
CREATE TABLE tw (a int) USING heap;
INSERT INTO tw VALUES (1);
CREATE TRIGGER b_after AFTER UPDATE ON tw FOR EACH ROW
  EXECUTE FUNCTION check_oid();
CREATE TRIGGER c_when AFTER UPDATE ON tw FOR EACH ROW
  WHEN (new.tableoid = old.tableoid AND new.tableoid <> 0)
  EXECUTE FUNCTION check_oid();
UPDATE tw SET a = a + 1;
CREATE TRIGGER a_before BEFORE UPDATE ON tw FOR EACH ROW
  EXECUTE FUNCTION check_oid();
UPDATE tw SET a = a + 1;
-- Transition tables contain row values, not system columns.
CREATE FUNCTION check_transition() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE 'transition: old %, new %',
    (SELECT array_agg(a ORDER BY a) FROM old_rows),
    (SELECT array_agg(a ORDER BY a) FROM new_rows);
  RETURN NULL;
END
$$;
CREATE TRIGGER d_transition AFTER UPDATE ON tw
  REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
  FOR EACH STATEMENT EXECUTE FUNCTION check_transition();
UPDATE tw SET a = a + 1;
SET search_path = semantics_flux, public;
CREATE TABLE m (tid int PRIMARY KEY, bal int) USING flux;
INSERT INTO m VALUES (1, 10);
MERGE INTO m USING (VALUES (1),(1)) s(sid) ON m.tid = s.sid
  WHEN MATCHED THEN UPDATE SET bal = 0;
INSERT INTO m VALUES (1, 1), (1, 2)
  ON CONFLICT (tid) DO UPDATE SET bal = 99;
TRUNCATE m;
INSERT INTO m VALUES (1, 1), (1, 2)
  ON CONFLICT (tid) DO UPDATE SET bal = 99;
CREATE TABLE cc (a int) USING flux;
BEGIN;
INSERT INTO cc VALUES (1);
INSERT INTO cc VALUES (2);
SELECT a, cmin, cmax, xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax FROM cc ORDER BY a;
UPDATE cc SET a = a + 10;
SELECT a, cmin, cmax, xmin = pg_current_xact_id()::text::xid AS own_xmin,
       xmax = '0'::xid AS no_xmax FROM cc ORDER BY a;
-- FLUX has no heap combo-CID word: old undo versions retain their
-- inserting CID, not heap's raw combo index. Test the live versions here.
INSERT INTO cc VALUES (3) RETURNING a, cmin, cmax;
UPDATE cc SET a = a + 10 WHERE a = 3 RETURNING a, cmin, cmax;
COMMIT;
CREATE FUNCTION check_oid() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE '%: old tableoid valid %, new tableoid valid %',
    TG_NAME, OLD.tableoid = TG_RELID, NEW.tableoid = TG_RELID;
  RETURN NEW;
END
$$;
CREATE TABLE tw (a int) USING flux;
INSERT INTO tw VALUES (1);
CREATE TRIGGER b_after AFTER UPDATE ON tw FOR EACH ROW
  EXECUTE FUNCTION check_oid();
CREATE TRIGGER c_when AFTER UPDATE ON tw FOR EACH ROW
  WHEN (new.tableoid = old.tableoid AND new.tableoid <> 0)
  EXECUTE FUNCTION check_oid();
UPDATE tw SET a = a + 1;
CREATE TRIGGER a_before BEFORE UPDATE ON tw FOR EACH ROW
  EXECUTE FUNCTION check_oid();
UPDATE tw SET a = a + 1;
-- Transition tables contain row values, not system columns.
CREATE FUNCTION check_transition() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE 'transition: old %, new %',
    (SELECT array_agg(a ORDER BY a) FROM old_rows),
    (SELECT array_agg(a ORDER BY a) FROM new_rows);
  RETURN NULL;
END
$$;
CREATE TRIGGER d_transition AFTER UPDATE ON tw
  REFERENCING OLD TABLE AS old_rows NEW TABLE AS new_rows
  FOR EACH STATEMENT EXECUTE FUNCTION check_transition();
UPDATE tw SET a = a + 1;
RESET search_path;
DROP SCHEMA semantics_heap CASCADE;
DROP SCHEMA semantics_flux CASCADE;
