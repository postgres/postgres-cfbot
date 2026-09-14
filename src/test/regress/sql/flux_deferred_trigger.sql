--
-- AFTER-trigger row events on FLUX tables whose row was rewritten in place
-- before the event fired: deferred constraint triggers, SET CONSTRAINTS,
-- triggers firing after a nested statement, transition tables and deferred
-- foreign keys.  Each case runs first on a heap twin (the control) and then
-- on FLUX; the two halves must print the same thing, and the final query
-- compares their table contents.
--
-- Heap re-fetches an event's row version by TID when the trigger fires, and
-- RI's check skips a version that was since superseded.  A FLUX row keeps its
-- TID across UPDATEs, so the event must carry its version.
--
CREATE SCHEMA dt_heap;
CREATE SCHEMA dt_flux;

CREATE FUNCTION public.dt_show_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  RAISE NOTICE '% % %: old % new %', TG_TABLE_NAME, TG_NAME, TG_OP, OLD, NEW;
  RETURN NULL;
END $$;
CREATE FUNCTION public.dt_fix_pid() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  IF NEW.pid = 99 THEN
    UPDATE nt SET pid = 1, v = v + 10 WHERE id = NEW.id;
  END IF;
  RETURN NULL;
END $$;
CREATE FUNCTION public.dt_show_transition() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE o text; n text;
BEGIN
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO o FROM oldtab t;
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO n FROM newtab t;
  RAISE NOTICE '% % % %: old % new %', TG_TABLE_NAME, TG_NAME, TG_OP, TG_LEVEL, o, n;
  RETURN NULL;
END $$;
CREATE FUNCTION public.dt_show_new_table() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE n text;
BEGIN
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO n FROM newtab t;
  RAISE NOTICE '% % %: new %', TG_TABLE_NAME, TG_NAME, TG_OP, n;
  RETURN NULL;
END $$;
CREATE FUNCTION public.dt_show_old_table() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE o text;
BEGIN
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO o FROM oldtab t;
  RAISE NOTICE '% % %: old %', TG_TABLE_NAME, TG_NAME, TG_OP, o;
  RETURN NULL;
END $$;

--
-- heap
--
\set am heap
\set schema dt_heap
SET default_table_access_method = :am;
SET search_path = :schema, public;

--
-- 1. A deferred constraint trigger sees each event's own OLD and NEW, even
-- after later statements rewrote the row: INSERT, UPDATE, UPDATE; then
-- UPDATE, DELETE.
--
CREATE TABLE dt (id int PRIMARY KEY, v int);
CREATE CONSTRAINT TRIGGER dt_show AFTER INSERT OR UPDATE OR DELETE ON dt
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION public.dt_show_row();
BEGIN;
INSERT INTO dt VALUES (1, 0);
UPDATE dt SET v = 1 WHERE id = 1;
UPDATE dt SET v = 2 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE dt SET v = 3 WHERE id = 1;
DELETE FROM dt WHERE id = 1;
COMMIT;

--
-- 2. The same, with SET CONSTRAINTS IMMEDIATE midway (fires what is queued),
-- then DEFERRED again.
--
BEGIN;
INSERT INTO dt VALUES (2, 0);
UPDATE dt SET v = 1 WHERE id = 2;
SET CONSTRAINTS dt_show IMMEDIATE;
UPDATE dt SET v = 2 WHERE id = 2;
SET CONSTRAINTS dt_show DEFERRED;
UPDATE dt SET v = 3 WHERE id = 2;
UPDATE dt SET v = 4 WHERE id = 2;
COMMIT;
SELECT * FROM dt ORDER BY id;

--
-- 3. Non-deferred AFTER triggers whose event fires after a later statement
-- rewrote the row: trigger "A_fix" (which fires first) re-updates the row in
-- a nested statement; "z_show" and the RI check on the outer event fire after
-- it.  The outer event's NEW is superseded by then, so heap's RI check skips
-- it.
--
CREATE TABLE np (id int PRIMARY KEY);
INSERT INTO np VALUES (1), (2);
CREATE TABLE nt (id int PRIMARY KEY, pid int REFERENCES np, v int);
CREATE TRIGGER "A_fix" AFTER UPDATE ON nt FOR EACH ROW
  EXECUTE FUNCTION public.dt_fix_pid();
CREATE TRIGGER z_show AFTER UPDATE ON nt FOR EACH ROW
  EXECUTE FUNCTION public.dt_show_row();
INSERT INTO nt VALUES (1, 1, 0), (2, 2, 0);
UPDATE nt SET pid = 99 WHERE id = 1;
-- a later statement's event on the twice-rewritten row
UPDATE nt SET v = v + 1 WHERE id = 1;
SELECT * FROM nt ORDER BY id;

--
-- 4. Transition tables, statement- and row-level, across statements of one
-- transaction.
--
CREATE TABLE tt (id int PRIMARY KEY, v int);
CREATE TRIGGER tt_stmt_ins AFTER INSERT ON tt REFERENCING NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_new_table();
CREATE TRIGGER tt_stmt_upd AFTER UPDATE ON tt REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_transition();
CREATE TRIGGER tt_stmt_del AFTER DELETE ON tt REFERENCING OLD TABLE AS oldtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_old_table();
CREATE TRIGGER tt_row_upd AFTER UPDATE ON tt REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH ROW EXECUTE FUNCTION public.dt_show_transition();
BEGIN;
INSERT INTO tt VALUES (1, 0), (2, 0), (3, 0);
UPDATE tt SET v = v + 1 WHERE id <= 2;
UPDATE tt SET v = v + 10;
DELETE FROM tt WHERE id = 1;
COMMIT;
SELECT * FROM tt ORDER BY id;

--
-- 5. Deferred FK checks of a row the transaction rewrote.  The check must
-- judge only the row's last version; each earlier one is superseded.
--
CREATE TABLE fp (id int PRIMARY KEY, v int);
CREATE TABLE fc (id int PRIMARY KEY, pid int REFERENCES fp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO fp VALUES (1, 0), (2, 0), (3, 0);
-- INSERT, UPDATE the FK column to a bad key, UPDATE it back: commits
BEGIN;
INSERT INTO fc VALUES (10, 1, 0);
UPDATE fc SET pid = 99 WHERE id = 10;
UPDATE fc SET pid = 1 WHERE id = 10;
COMMIT;
-- a bad INSERT fixed, then broken again: fails on the last key, 98
BEGIN;
INSERT INTO fc VALUES (11, 99, 0);
UPDATE fc SET pid = 1 WHERE id = 11;
UPDATE fc SET pid = 98 WHERE id = 11;
COMMIT;
-- a bad key, then a non-key UPDATE: fails on 97
BEGIN;
UPDATE fc SET pid = 97 WHERE id = 10;
UPDATE fc SET w = 1 WHERE id = 10;
COMMIT;
-- a bad key, then the row deleted: commits
BEGIN;
INSERT INTO fc VALUES (12, 96, 0);
UPDATE fc SET pid = 95 WHERE id = 12;
DELETE FROM fc WHERE id = 12;
COMMIT;
-- SET CONSTRAINTS ALL IMMEDIATE midway, after the bad key was fixed: commits
BEGIN;
INSERT INTO fc VALUES (13, 94, 0);
UPDATE fc SET pid = 2 WHERE id = 13;
SET CONSTRAINTS ALL IMMEDIATE;
UPDATE fc SET pid = 3 WHERE id = 13;
COMMIT;
-- ... and before it was fixed: fails at SET CONSTRAINTS, on 93
BEGIN;
INSERT INTO fc VALUES (14, 93, 0);
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
-- the bad key's fix made in a subtransaction that commits: commits
BEGIN;
INSERT INTO fc VALUES (15, 92, 0);
SAVEPOINT s;
UPDATE fc SET pid = 1 WHERE id = 15;
RELEASE SAVEPOINT s;
COMMIT;
-- a good key's breakage made in a subtransaction that rolls back: commits
BEGIN;
INSERT INTO fc VALUES (16, 1, 0);
UPDATE fc SET w = 1 WHERE id = 16;
SAVEPOINT s;
UPDATE fc SET pid = 91 WHERE id = 16;
ROLLBACK TO SAVEPOINT s;
COMMIT;
-- the same, then broken for real after the rollback: fails on 90
BEGIN;
UPDATE fc SET w = 2 WHERE id = 16;
SAVEPOINT s;
UPDATE fc SET pid = 1 WHERE id = 16;
ROLLBACK TO SAVEPOINT s;
UPDATE fc SET pid = 90 WHERE id = 16;
COMMIT;
SELECT * FROM fc ORDER BY id;

--
-- 6. A deferred FK check whose parent row is updated in place between the
-- check being queued and firing.  Each case has its own parent row, so no
-- earlier case's row locks or aborted versions are in its history.
--
CREATE TABLE gp (id int PRIMARY KEY, v int);
CREATE TABLE gc (id int PRIMARY KEY, pid int REFERENCES gp DEFERRABLE INITIALLY DEFERRED);
INSERT INTO gp VALUES (1, 0), (2, 0), (3, 0), (4, 0), (5, 0);
INSERT INTO gc VALUES (40, 4), (50, 5);
-- a non-key column of the parent: commits
BEGIN;
INSERT INTO gc VALUES (10, 1);
UPDATE gp SET v = 5 WHERE id = 1;
COMMIT;
-- the parent's key moved away: fails
BEGIN;
INSERT INTO gc VALUES (20, 2);
UPDATE gp SET id = 20 WHERE id = 2;
COMMIT;
-- the parent's key moved away and back: commits
BEGIN;
INSERT INTO gc VALUES (30, 3);
UPDATE gp SET id = 30 WHERE id = 3;
UPDATE gp SET v = 6 WHERE id = 30;
UPDATE gp SET id = 3 WHERE id = 30;
COMMIT;
-- a referenced parent's key moved away and back (the parent's own deferred
-- NO ACTION check): commits
BEGIN;
UPDATE gp SET id = 41 WHERE id = 4;
UPDATE gp SET id = 4 WHERE id = 41;
COMMIT;
-- ... and moved away for good: fails
BEGIN;
UPDATE gp SET id = 51 WHERE id = 5;
UPDATE gp SET v = 7 WHERE id = 51;
COMMIT;
SELECT * FROM gp ORDER BY id;
SELECT * FROM gc ORDER BY id;

--
-- 7. The original report (stale.sql).
--
CREATE TABLE sp (id int PRIMARY KEY);
CREATE TABLE sc (id int PRIMARY KEY, pid int REFERENCES sp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO sp VALUES (1), (2);
INSERT INTO sc VALUES (1, 1, 0);
BEGIN;
INSERT INTO sc VALUES (2, 99, 0);
UPDATE sc SET pid = 1 WHERE id = 2;
COMMIT;
BEGIN;
UPDATE sc SET pid = 95 WHERE id = 1;
UPDATE sc SET w = 6 WHERE id = 1;
UPDATE sc SET pid = 1 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE sc SET pid = 94 WHERE id = 2;
UPDATE sc SET pid = 2 WHERE id = 2;
UPDATE sc SET pid = 93, w = 1 WHERE id = 2;
COMMIT;
SELECT * FROM sc ORDER BY id;

--
-- flux
--
\set am flux
\set schema dt_flux
SET default_table_access_method = :am;
SET search_path = :schema, public;

--
-- 1. A deferred constraint trigger sees each event's own OLD and NEW, even
-- after later statements rewrote the row: INSERT, UPDATE, UPDATE; then
-- UPDATE, DELETE.
--
CREATE TABLE dt (id int PRIMARY KEY, v int);
CREATE CONSTRAINT TRIGGER dt_show AFTER INSERT OR UPDATE OR DELETE ON dt
  DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION public.dt_show_row();
BEGIN;
INSERT INTO dt VALUES (1, 0);
UPDATE dt SET v = 1 WHERE id = 1;
UPDATE dt SET v = 2 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE dt SET v = 3 WHERE id = 1;
DELETE FROM dt WHERE id = 1;
COMMIT;

--
-- 2. The same, with SET CONSTRAINTS IMMEDIATE midway (fires what is queued),
-- then DEFERRED again.
--
BEGIN;
INSERT INTO dt VALUES (2, 0);
UPDATE dt SET v = 1 WHERE id = 2;
SET CONSTRAINTS dt_show IMMEDIATE;
UPDATE dt SET v = 2 WHERE id = 2;
SET CONSTRAINTS dt_show DEFERRED;
UPDATE dt SET v = 3 WHERE id = 2;
UPDATE dt SET v = 4 WHERE id = 2;
COMMIT;
SELECT * FROM dt ORDER BY id;

--
-- 3. Non-deferred AFTER triggers whose event fires after a later statement
-- rewrote the row: trigger "A_fix" (which fires first) re-updates the row in
-- a nested statement; "z_show" and the RI check on the outer event fire after
-- it.  The outer event's NEW is superseded by then, so heap's RI check skips
-- it.
--
CREATE TABLE np (id int PRIMARY KEY);
INSERT INTO np VALUES (1), (2);
CREATE TABLE nt (id int PRIMARY KEY, pid int REFERENCES np, v int);
CREATE TRIGGER "A_fix" AFTER UPDATE ON nt FOR EACH ROW
  EXECUTE FUNCTION public.dt_fix_pid();
CREATE TRIGGER z_show AFTER UPDATE ON nt FOR EACH ROW
  EXECUTE FUNCTION public.dt_show_row();
INSERT INTO nt VALUES (1, 1, 0), (2, 2, 0);
UPDATE nt SET pid = 99 WHERE id = 1;
-- a later statement's event on the twice-rewritten row
UPDATE nt SET v = v + 1 WHERE id = 1;
SELECT * FROM nt ORDER BY id;

--
-- 4. Transition tables, statement- and row-level, across statements of one
-- transaction.
--
CREATE TABLE tt (id int PRIMARY KEY, v int);
CREATE TRIGGER tt_stmt_ins AFTER INSERT ON tt REFERENCING NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_new_table();
CREATE TRIGGER tt_stmt_upd AFTER UPDATE ON tt REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_transition();
CREATE TRIGGER tt_stmt_del AFTER DELETE ON tt REFERENCING OLD TABLE AS oldtab
  FOR EACH STATEMENT EXECUTE FUNCTION public.dt_show_old_table();
CREATE TRIGGER tt_row_upd AFTER UPDATE ON tt REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH ROW EXECUTE FUNCTION public.dt_show_transition();
BEGIN;
INSERT INTO tt VALUES (1, 0), (2, 0), (3, 0);
UPDATE tt SET v = v + 1 WHERE id <= 2;
UPDATE tt SET v = v + 10;
DELETE FROM tt WHERE id = 1;
COMMIT;
SELECT * FROM tt ORDER BY id;

--
-- 5. Deferred FK checks of a row the transaction rewrote.  The check must
-- judge only the row's last version; each earlier one is superseded.
--
CREATE TABLE fp (id int PRIMARY KEY, v int);
CREATE TABLE fc (id int PRIMARY KEY, pid int REFERENCES fp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO fp VALUES (1, 0), (2, 0), (3, 0);
-- INSERT, UPDATE the FK column to a bad key, UPDATE it back: commits
BEGIN;
INSERT INTO fc VALUES (10, 1, 0);
UPDATE fc SET pid = 99 WHERE id = 10;
UPDATE fc SET pid = 1 WHERE id = 10;
COMMIT;
-- a bad INSERT fixed, then broken again: fails on the last key, 98
BEGIN;
INSERT INTO fc VALUES (11, 99, 0);
UPDATE fc SET pid = 1 WHERE id = 11;
UPDATE fc SET pid = 98 WHERE id = 11;
COMMIT;
-- a bad key, then a non-key UPDATE: fails on 97
BEGIN;
UPDATE fc SET pid = 97 WHERE id = 10;
UPDATE fc SET w = 1 WHERE id = 10;
COMMIT;
-- a bad key, then the row deleted: commits
BEGIN;
INSERT INTO fc VALUES (12, 96, 0);
UPDATE fc SET pid = 95 WHERE id = 12;
DELETE FROM fc WHERE id = 12;
COMMIT;
-- SET CONSTRAINTS ALL IMMEDIATE midway, after the bad key was fixed: commits
BEGIN;
INSERT INTO fc VALUES (13, 94, 0);
UPDATE fc SET pid = 2 WHERE id = 13;
SET CONSTRAINTS ALL IMMEDIATE;
UPDATE fc SET pid = 3 WHERE id = 13;
COMMIT;
-- ... and before it was fixed: fails at SET CONSTRAINTS, on 93
BEGIN;
INSERT INTO fc VALUES (14, 93, 0);
SET CONSTRAINTS ALL IMMEDIATE;
ROLLBACK;
-- the bad key's fix made in a subtransaction that commits: commits
BEGIN;
INSERT INTO fc VALUES (15, 92, 0);
SAVEPOINT s;
UPDATE fc SET pid = 1 WHERE id = 15;
RELEASE SAVEPOINT s;
COMMIT;
-- a good key's breakage made in a subtransaction that rolls back: commits
BEGIN;
INSERT INTO fc VALUES (16, 1, 0);
UPDATE fc SET w = 1 WHERE id = 16;
SAVEPOINT s;
UPDATE fc SET pid = 91 WHERE id = 16;
ROLLBACK TO SAVEPOINT s;
COMMIT;
-- the same, then broken for real after the rollback: fails on 90
BEGIN;
UPDATE fc SET w = 2 WHERE id = 16;
SAVEPOINT s;
UPDATE fc SET pid = 1 WHERE id = 16;
ROLLBACK TO SAVEPOINT s;
UPDATE fc SET pid = 90 WHERE id = 16;
COMMIT;
SELECT * FROM fc ORDER BY id;

--
-- 6. A deferred FK check whose parent row is updated in place between the
-- check being queued and firing.  Each case has its own parent row, so no
-- earlier case's row locks or aborted versions are in its history.
--
CREATE TABLE gp (id int PRIMARY KEY, v int);
CREATE TABLE gc (id int PRIMARY KEY, pid int REFERENCES gp DEFERRABLE INITIALLY DEFERRED);
INSERT INTO gp VALUES (1, 0), (2, 0), (3, 0), (4, 0), (5, 0);
INSERT INTO gc VALUES (40, 4), (50, 5);
-- a non-key column of the parent: commits
BEGIN;
INSERT INTO gc VALUES (10, 1);
UPDATE gp SET v = 5 WHERE id = 1;
COMMIT;
-- the parent's key moved away: fails
BEGIN;
INSERT INTO gc VALUES (20, 2);
UPDATE gp SET id = 20 WHERE id = 2;
COMMIT;
-- the parent's key moved away and back: commits
BEGIN;
INSERT INTO gc VALUES (30, 3);
UPDATE gp SET id = 30 WHERE id = 3;
UPDATE gp SET v = 6 WHERE id = 30;
UPDATE gp SET id = 3 WHERE id = 30;
COMMIT;
-- a referenced parent's key moved away and back (the parent's own deferred
-- NO ACTION check): commits
BEGIN;
UPDATE gp SET id = 41 WHERE id = 4;
UPDATE gp SET id = 4 WHERE id = 41;
COMMIT;
-- ... and moved away for good: fails
BEGIN;
UPDATE gp SET id = 51 WHERE id = 5;
UPDATE gp SET v = 7 WHERE id = 51;
COMMIT;
SELECT * FROM gp ORDER BY id;
SELECT * FROM gc ORDER BY id;

--
-- 7. The original report (stale.sql).
--
CREATE TABLE sp (id int PRIMARY KEY);
CREATE TABLE sc (id int PRIMARY KEY, pid int REFERENCES sp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO sp VALUES (1), (2);
INSERT INTO sc VALUES (1, 1, 0);
BEGIN;
INSERT INTO sc VALUES (2, 99, 0);
UPDATE sc SET pid = 1 WHERE id = 2;
COMMIT;
BEGIN;
UPDATE sc SET pid = 95 WHERE id = 1;
UPDATE sc SET w = 6 WHERE id = 1;
UPDATE sc SET pid = 1 WHERE id = 1;
COMMIT;
BEGIN;
UPDATE sc SET pid = 94 WHERE id = 2;
UPDATE sc SET pid = 2 WHERE id = 2;
UPDATE sc SET pid = 93, w = 1 WHERE id = 2;
COMMIT;
SELECT * FROM sc ORDER BY id;

--
-- The twins must agree on every table's contents.
--
RESET search_path;
RESET default_table_access_method;
CREATE FUNCTION pg_temp.dt_twin_diff(t text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  EXECUTE format('SELECT count(*) FROM ((SELECT * FROM dt_heap.%1$I EXCEPT ALL SELECT * FROM dt_flux.%1$I)'
                 ' UNION ALL (SELECT * FROM dt_flux.%1$I EXCEPT ALL SELECT * FROM dt_heap.%1$I)) d', t)
    INTO n;
  RETURN n;
END $$;
SELECT c.relname, pg_temp.dt_twin_diff(c.relname) AS differing_rows
  FROM pg_class c
  WHERE c.relnamespace = 'dt_heap'::regnamespace AND c.relkind = 'r'
  ORDER BY 1;
SELECT c.relname, a.amname
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.relnamespace = 'dt_flux'::regnamespace AND c.relkind = 'r'
  ORDER BY 1;

DROP SCHEMA dt_heap CASCADE;
DROP SCHEMA dt_flux CASCADE;
DROP FUNCTION public.dt_show_row(), public.dt_fix_pid(), public.dt_show_transition(),
  public.dt_show_new_table(), public.dt_show_old_table();
