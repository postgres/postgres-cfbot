--
-- Foreign keys and AFTER triggers on FLUX tables, each case run first on a
-- heap twin (the control) and then on FLUX.  The two halves must print the
-- same thing; the final query compares their table contents.
--
-- An AM that updates rows in place hands the AFTER-trigger machinery copies
-- of the old and new row images, and the RI triggers ask such a copy whether
-- the current transaction created it (slot_is_current_xact_tuple).
--
CREATE SCHEMA fk_heap;
CREATE SCHEMA fk_flux;

CREATE FUNCTION public.fk_log_row() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  INSERT INTO log VALUES (nextval('log_seq'), TG_TABLE_NAME,
                          TG_NAME || ' ' || TG_OP, OLD::text, NEW::text);
  RETURN NULL;
END $$;
CREATE FUNCTION public.fk_show_transition() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE o text; n text;
BEGIN
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO o FROM oldtab t;
  SELECT string_agg(t::text, ' ' ORDER BY t::text) INTO n FROM newtab t;
  RAISE NOTICE '% %: old % new %', TG_TABLE_NAME, TG_OP, o, n;
  RETURN NULL;
END $$;
CREATE FUNCTION public.fk_bump_w() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
  NEW.w := NEW.w + 100;
  IF NEW.w = 99 THEN NEW.pid := 8; END IF;
  RETURN NEW;
END $$;

--
-- heap
--
\set am heap
\set other flux
\set schema fk_heap
SET default_table_access_method = :am;
SET search_path = :schema, public;

-- Event log for the row triggers below; compared across the twins at the end.
CREATE TABLE log (seq int, tbl text, op text, old_row text, new_row text);
CREATE SEQUENCE log_seq;

--
-- 1. Child UPDATE, of a non-FK column and of the FK column.  The second
-- block's row was inserted by this transaction (the is_current_xact_tuple
-- branch of RI_FKey_fk_upd_check_required).
--
CREATE TABLE pp (id int PRIMARY KEY, v int);
INSERT INTO pp VALUES (1, 0), (2, 0);
CREATE TABLE pc (id int PRIMARY KEY, pid int REFERENCES pp, w int);
INSERT INTO pc VALUES (1, 1, 0), (2, 2, 0);
UPDATE pc SET w = 5 WHERE id = 1;
UPDATE pc SET pid = 2 WHERE id = 1;
UPDATE pc SET pid = 3 WHERE id = 1;
UPDATE pc SET w = w + 1;
BEGIN;
INSERT INTO pc VALUES (3, 1, 0);
UPDATE pc SET w = 1 WHERE id = 3;
UPDATE pc SET pid = 2, w = 2 WHERE id = 3;
COMMIT;
BEGIN;
INSERT INTO pc VALUES (4, 1, 0);
UPDATE pc SET pid = 3 WHERE id = 4;
ROLLBACK;
SELECT * FROM pc ORDER BY id;

--
-- 2. Parent UPDATE of a non-key column and of the key, under each action.
-- Child rows reference parent 1; parent 2 is unreferenced.
--
CREATE TABLE p_na (id int PRIMARY KEY, v int);
CREATE TABLE c_na (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_na ON UPDATE NO ACTION, w int);
CREATE TABLE p_re (id int PRIMARY KEY, v int);
CREATE TABLE c_re (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_re ON UPDATE RESTRICT, w int);
CREATE TABLE p_ca (id int PRIMARY KEY, v int);
CREATE TABLE c_ca (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_ca ON UPDATE CASCADE, w int);
CREATE TABLE p_sn (id int PRIMARY KEY, v int);
CREATE TABLE c_sn (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_sn ON UPDATE SET NULL, w int);
CREATE TABLE p_sd (id int PRIMARY KEY, v int);
CREATE TABLE c_sd (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_sd ON UPDATE SET DEFAULT, w int);
INSERT INTO p_na VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_re VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_ca VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_sn VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_sd VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO c_na VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_re VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_ca VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_sn VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_sd VALUES (1, 1, 0), (2, 1, 0);
UPDATE p_na SET v = 1 WHERE id = 1;
UPDATE p_na SET id = 10 WHERE id = 1;
UPDATE p_na SET id = 20 WHERE id = 2;
UPDATE p_re SET v = 1 WHERE id = 1;
UPDATE p_re SET id = 10 WHERE id = 1;
UPDATE p_re SET id = 20 WHERE id = 2;
UPDATE p_ca SET v = 1 WHERE id = 1;
UPDATE p_ca SET id = 10 WHERE id = 1;
UPDATE p_ca SET id = 20 WHERE id = 2;
UPDATE p_ca SET id = 11 WHERE id = 10;
UPDATE p_sn SET v = 1 WHERE id = 1;
UPDATE p_sn SET id = 10 WHERE id = 1;
UPDATE p_sn SET id = 20 WHERE id = 2;
UPDATE p_sd SET v = 1 WHERE id = 1;
UPDATE p_sd SET id = 10 WHERE id = 1;
UPDATE p_sd SET id = 20 WHERE id = 2;
UPDATE p_sd SET id = 30 WHERE id = 0;
SELECT 'na' AS t, * FROM c_na UNION ALL SELECT 're', * FROM c_re
  UNION ALL SELECT 'ca', * FROM c_ca UNION ALL SELECT 'sn', * FROM c_sn
  UNION ALL SELECT 'sd', * FROM c_sd ORDER BY 1, 2;
-- the cascaded rows can be updated again, and the cascade repeated
UPDATE c_ca SET w = 7;
UPDATE p_ca SET id = 12 WHERE id = 11;
SELECT * FROM c_ca ORDER BY id;

--
-- 3. DELETE with ON DELETE CASCADE and SET NULL.
--
CREATE TABLE dp (id int PRIMARY KEY);
CREATE TABLE dc_ca (id int PRIMARY KEY, pid int REFERENCES dp ON DELETE CASCADE, w int);
CREATE TABLE dc_sn (id int PRIMARY KEY, pid int REFERENCES dp ON DELETE SET NULL, w int);
INSERT INTO dp VALUES (1), (2);
INSERT INTO dc_ca VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
INSERT INTO dc_sn VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
UPDATE dc_ca SET w = 1;
UPDATE dc_sn SET w = 1;
DELETE FROM dp WHERE id = 1;
UPDATE dc_sn SET w = 2 WHERE pid IS NULL;
UPDATE dc_sn SET pid = 2 WHERE id = 1;
SELECT 'ca' AS t, * FROM dc_ca UNION ALL SELECT 'sn', * FROM dc_sn ORDER BY 1, 2;

--
-- 4. Deferrable FKs.
--
CREATE TABLE fp (id int PRIMARY KEY);
CREATE TABLE fc (id int PRIMARY KEY, pid int REFERENCES fp ON UPDATE CASCADE DEFERRABLE INITIALLY DEFERRED, w int);
CREATE TABLE fc_na (id int PRIMARY KEY, pid int REFERENCES fp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO fp VALUES (1);
INSERT INTO fc VALUES (1, 1, 0);
INSERT INTO fc_na VALUES (1, 1, 0);
-- the parent row appears later in the transaction
BEGIN;
UPDATE fc SET w = 1 WHERE id = 1;
UPDATE fc SET pid = 2 WHERE id = 1;
INSERT INTO fp VALUES (2);
COMMIT;
-- a row inserted, then updated, with a good key throughout
BEGIN;
INSERT INTO fc VALUES (2, 1, 0);
UPDATE fc SET w = 1 WHERE id = 2;
UPDATE fc SET pid = 2 WHERE id = 2;
COMMIT;
-- a bad key that is never fixed
BEGIN;
UPDATE fc SET pid = 98 WHERE id = 1;
COMMIT;
-- a bad key, then an UPDATE of another column of the same row
BEGIN;
UPDATE fc SET pid = 96 WHERE id = 1;
UPDATE fc SET w = 5 WHERE id = 1;
COMMIT;
-- a row inserted and then updated without touching its (bad) key
BEGIN;
INSERT INTO fc VALUES (3, 92, 0);
UPDATE fc SET w = 1 WHERE id = 3;
COMMIT;
-- a parent key changed and changed back (NO ACTION) is fine at commit
BEGIN;
UPDATE fp SET id = 3 WHERE id = 1;
UPDATE fp SET id = 1 WHERE id = 3;
COMMIT;
-- the same, checked immediately
BEGIN;
SET CONSTRAINTS ALL IMMEDIATE;
UPDATE fc SET pid = 97 WHERE id = 1;
ROLLBACK;
SELECT 'fc' AS t, * FROM fc UNION ALL SELECT 'fc_na', * FROM fc_na ORDER BY 1, 2;

--
-- 5. A self-referencing FK.
--
CREATE TABLE tree (id int PRIMARY KEY, parent int REFERENCES tree ON UPDATE CASCADE ON DELETE CASCADE, w int);
INSERT INTO tree VALUES (1, NULL, 0), (2, 1, 0), (3, 2, 0), (4, 1, 0);
UPDATE tree SET w = 1;
UPDATE tree SET id = 10 WHERE id = 1;
UPDATE tree SET id = 20 WHERE id = 2;
UPDATE tree SET parent = 4 WHERE id = 3;
UPDATE tree SET parent = 5 WHERE id = 3;
SELECT * FROM tree ORDER BY id;
DELETE FROM tree WHERE id = 10;
SELECT count(*) FROM tree;

--
-- 6. An FK across access methods, in both directions.
--
CREATE TABLE xp_o (id int PRIMARY KEY, v int) USING :other;
CREATE TABLE xc_d (id int PRIMARY KEY, pid int REFERENCES xp_o ON UPDATE CASCADE ON DELETE SET NULL, w int);
CREATE TABLE xp_d (id int PRIMARY KEY, v int);
CREATE TABLE xc_o (id int PRIMARY KEY, pid int REFERENCES xp_d ON UPDATE CASCADE ON DELETE SET NULL, w int) USING :other;
INSERT INTO xp_o VALUES (1, 0), (2, 0);
INSERT INTO xp_d VALUES (1, 0), (2, 0);
INSERT INTO xc_d VALUES (1, 1, 0), (2, 2, 0);
INSERT INTO xc_o VALUES (1, 1, 0), (2, 2, 0);
UPDATE xc_d SET w = 1;
UPDATE xc_o SET w = 1;
UPDATE xc_d SET pid = 2 WHERE id = 1;
UPDATE xc_o SET pid = 2 WHERE id = 1;
UPDATE xc_d SET pid = 3 WHERE id = 1;
UPDATE xc_o SET pid = 3 WHERE id = 1;
UPDATE xp_o SET v = 1;
UPDATE xp_d SET v = 1;
UPDATE xp_o SET id = 5 WHERE id = 2;
UPDATE xp_d SET id = 5 WHERE id = 2;
DELETE FROM xp_o WHERE id = 1;
DELETE FROM xp_d WHERE id = 1;
SELECT 'xc_d' AS t, * FROM xc_d UNION ALL SELECT 'xc_o', * FROM xc_o ORDER BY 1, 2;

--
-- 7. Partitioned tables, and 9. UPDATEs that move a row across partitions.
--
CREATE TABLE ptp (id int PRIMARY KEY, v int) PARTITION BY RANGE (id);
CREATE TABLE ptp1 PARTITION OF ptp FOR VALUES FROM (0) TO (100);
CREATE TABLE ptp2 PARTITION OF ptp FOR VALUES FROM (100) TO (200);
CREATE TABLE ptc (id int PRIMARY KEY, pid int REFERENCES ptp ON UPDATE CASCADE ON DELETE SET NULL, w int) PARTITION BY RANGE (id);
CREATE TABLE ptc1 PARTITION OF ptc FOR VALUES FROM (0) TO (100);
CREATE TABLE ptc2 PARTITION OF ptc FOR VALUES FROM (100) TO (200);
INSERT INTO ptp VALUES (1, 0), (2, 0), (150, 0);
INSERT INTO ptc VALUES (1, 1, 0), (2, 2, 0), (101, 150, 0);
UPDATE ptc SET w = 1;
UPDATE ptc SET pid = 2 WHERE id = 1;
UPDATE ptc SET pid = 3 WHERE id = 1;
UPDATE ptp SET v = 1;
UPDATE ptp SET id = 3 WHERE id = 2;
UPDATE ptp SET id = 160 WHERE id = 150;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;
-- parent row moves partition; its children follow by cascade
UPDATE ptp SET id = 120 WHERE id = 3;
-- child row moves partition, with and without an FK column change
UPDATE ptc SET id = 150 WHERE id = 1;
UPDATE ptc SET id = 50, pid = 160 WHERE id = 101;
UPDATE ptc SET id = 151, pid = 7 WHERE id = 150;
SELECT tableoid::regclass, * FROM ptp ORDER BY id;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;
DELETE FROM ptp WHERE id = 120;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;

--
-- 8. MERGE on FK tables, child and parent side.
--
MERGE INTO pc USING (VALUES (1, 1, 7), (5, 2, 0), (2, NULL, 9)) s(id, pid, w)
  ON pc.id = s.id
  WHEN MATCHED AND s.pid IS NULL THEN UPDATE SET w = s.w
  WHEN MATCHED THEN UPDATE SET pid = s.pid, w = s.w
  WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.pid, s.w);
MERGE INTO pc USING (VALUES (1, 4)) s(id, pid) ON pc.id = s.id
  WHEN MATCHED THEN UPDATE SET pid = s.pid;
SELECT * FROM pc ORDER BY id;
MERGE INTO p_ca USING (VALUES (12, 13), (20, 21)) s(id, nid) ON p_ca.id = s.id
  WHEN MATCHED THEN UPDATE SET id = s.nid;
SELECT * FROM c_ca ORDER BY id;
MERGE INTO dp USING (VALUES (2)) s(id) ON dp.id = s.id
  WHEN MATCHED THEN DELETE;
SELECT 'ca' AS t, * FROM dc_ca UNION ALL SELECT 'sn', * FROM dc_sn ORDER BY 1, 2;

--
-- 10. Other AFTER-trigger users of the old/new slot: a row trigger that
-- reads OLD and NEW (immediate and deferred), transition tables, and a
-- BEFORE trigger combined with an FK.
--
CREATE TABLE tp (id int PRIMARY KEY, v int);
CREATE TABLE tc (id int PRIMARY KEY, pid int REFERENCES tp ON UPDATE CASCADE, w int);
INSERT INTO tp VALUES (1, 0), (2, 0);
INSERT INTO tc VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
CREATE TRIGGER tc_after AFTER UPDATE ON tc
  FOR EACH ROW EXECUTE FUNCTION fk_log_row();
CREATE CONSTRAINT TRIGGER tc_deferred AFTER UPDATE ON tc
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE FUNCTION fk_log_row();
CREATE TRIGGER tc_trans AFTER UPDATE ON tc
  REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION fk_show_transition();
CREATE TRIGGER tp_trans AFTER UPDATE ON tp
  REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION fk_show_transition();
CREATE TRIGGER tc_before BEFORE UPDATE ON tc
  FOR EACH ROW EXECUTE FUNCTION fk_bump_w();
UPDATE tc SET w = 5 WHERE id = 1;
UPDATE tc SET pid = 2 WHERE id = 1;
UPDATE tc SET pid = 9 WHERE id = 1;
UPDATE tp SET id = 10 WHERE id = 1;
BEGIN;
UPDATE tc SET w = 0 WHERE id = 2;
UPDATE tc SET w = 1 WHERE id = 2;
UPDATE tc SET pid = 10 WHERE id = 2;
COMMIT;
-- the BEFORE trigger redirects the FK to a missing parent
UPDATE tc SET w = -1 WHERE id = 2;
SELECT * FROM tc ORDER BY id;
SELECT tbl, op, old_row, new_row FROM log ORDER BY seq;

--
-- flux
--
\set am flux
\set other heap
\set schema fk_flux
SET default_table_access_method = :am;
SET search_path = :schema, public;

-- Event log for the row triggers below; compared across the twins at the end.
CREATE TABLE log (seq int, tbl text, op text, old_row text, new_row text);
CREATE SEQUENCE log_seq;

--
-- 1. Child UPDATE, of a non-FK column and of the FK column.  The second
-- block's row was inserted by this transaction (the is_current_xact_tuple
-- branch of RI_FKey_fk_upd_check_required).
--
CREATE TABLE pp (id int PRIMARY KEY, v int);
INSERT INTO pp VALUES (1, 0), (2, 0);
CREATE TABLE pc (id int PRIMARY KEY, pid int REFERENCES pp, w int);
INSERT INTO pc VALUES (1, 1, 0), (2, 2, 0);
UPDATE pc SET w = 5 WHERE id = 1;
UPDATE pc SET pid = 2 WHERE id = 1;
UPDATE pc SET pid = 3 WHERE id = 1;
UPDATE pc SET w = w + 1;
BEGIN;
INSERT INTO pc VALUES (3, 1, 0);
UPDATE pc SET w = 1 WHERE id = 3;
UPDATE pc SET pid = 2, w = 2 WHERE id = 3;
COMMIT;
BEGIN;
INSERT INTO pc VALUES (4, 1, 0);
UPDATE pc SET pid = 3 WHERE id = 4;
ROLLBACK;
SELECT * FROM pc ORDER BY id;

--
-- 2. Parent UPDATE of a non-key column and of the key, under each action.
-- Child rows reference parent 1; parent 2 is unreferenced.
--
CREATE TABLE p_na (id int PRIMARY KEY, v int);
CREATE TABLE c_na (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_na ON UPDATE NO ACTION, w int);
CREATE TABLE p_re (id int PRIMARY KEY, v int);
CREATE TABLE c_re (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_re ON UPDATE RESTRICT, w int);
CREATE TABLE p_ca (id int PRIMARY KEY, v int);
CREATE TABLE c_ca (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_ca ON UPDATE CASCADE, w int);
CREATE TABLE p_sn (id int PRIMARY KEY, v int);
CREATE TABLE c_sn (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_sn ON UPDATE SET NULL, w int);
CREATE TABLE p_sd (id int PRIMARY KEY, v int);
CREATE TABLE c_sd (id int PRIMARY KEY, pid int DEFAULT 0 REFERENCES p_sd ON UPDATE SET DEFAULT, w int);
INSERT INTO p_na VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_re VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_ca VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_sn VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO p_sd VALUES (0, 0), (1, 0), (2, 0);
INSERT INTO c_na VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_re VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_ca VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_sn VALUES (1, 1, 0), (2, 1, 0);
INSERT INTO c_sd VALUES (1, 1, 0), (2, 1, 0);
UPDATE p_na SET v = 1 WHERE id = 1;
UPDATE p_na SET id = 10 WHERE id = 1;
UPDATE p_na SET id = 20 WHERE id = 2;
UPDATE p_re SET v = 1 WHERE id = 1;
UPDATE p_re SET id = 10 WHERE id = 1;
UPDATE p_re SET id = 20 WHERE id = 2;
UPDATE p_ca SET v = 1 WHERE id = 1;
UPDATE p_ca SET id = 10 WHERE id = 1;
UPDATE p_ca SET id = 20 WHERE id = 2;
UPDATE p_ca SET id = 11 WHERE id = 10;
UPDATE p_sn SET v = 1 WHERE id = 1;
UPDATE p_sn SET id = 10 WHERE id = 1;
UPDATE p_sn SET id = 20 WHERE id = 2;
UPDATE p_sd SET v = 1 WHERE id = 1;
UPDATE p_sd SET id = 10 WHERE id = 1;
UPDATE p_sd SET id = 20 WHERE id = 2;
UPDATE p_sd SET id = 30 WHERE id = 0;
SELECT 'na' AS t, * FROM c_na UNION ALL SELECT 're', * FROM c_re
  UNION ALL SELECT 'ca', * FROM c_ca UNION ALL SELECT 'sn', * FROM c_sn
  UNION ALL SELECT 'sd', * FROM c_sd ORDER BY 1, 2;
-- the cascaded rows can be updated again, and the cascade repeated
UPDATE c_ca SET w = 7;
UPDATE p_ca SET id = 12 WHERE id = 11;
SELECT * FROM c_ca ORDER BY id;

--
-- 3. DELETE with ON DELETE CASCADE and SET NULL.
--
CREATE TABLE dp (id int PRIMARY KEY);
CREATE TABLE dc_ca (id int PRIMARY KEY, pid int REFERENCES dp ON DELETE CASCADE, w int);
CREATE TABLE dc_sn (id int PRIMARY KEY, pid int REFERENCES dp ON DELETE SET NULL, w int);
INSERT INTO dp VALUES (1), (2);
INSERT INTO dc_ca VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
INSERT INTO dc_sn VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
UPDATE dc_ca SET w = 1;
UPDATE dc_sn SET w = 1;
DELETE FROM dp WHERE id = 1;
UPDATE dc_sn SET w = 2 WHERE pid IS NULL;
UPDATE dc_sn SET pid = 2 WHERE id = 1;
SELECT 'ca' AS t, * FROM dc_ca UNION ALL SELECT 'sn', * FROM dc_sn ORDER BY 1, 2;

--
-- 4. Deferrable FKs.
--
CREATE TABLE fp (id int PRIMARY KEY);
CREATE TABLE fc (id int PRIMARY KEY, pid int REFERENCES fp ON UPDATE CASCADE DEFERRABLE INITIALLY DEFERRED, w int);
CREATE TABLE fc_na (id int PRIMARY KEY, pid int REFERENCES fp DEFERRABLE INITIALLY DEFERRED, w int);
INSERT INTO fp VALUES (1);
INSERT INTO fc VALUES (1, 1, 0);
INSERT INTO fc_na VALUES (1, 1, 0);
-- the parent row appears later in the transaction
BEGIN;
UPDATE fc SET w = 1 WHERE id = 1;
UPDATE fc SET pid = 2 WHERE id = 1;
INSERT INTO fp VALUES (2);
COMMIT;
-- a row inserted, then updated, with a good key throughout
BEGIN;
INSERT INTO fc VALUES (2, 1, 0);
UPDATE fc SET w = 1 WHERE id = 2;
UPDATE fc SET pid = 2 WHERE id = 2;
COMMIT;
-- a bad key that is never fixed
BEGIN;
UPDATE fc SET pid = 98 WHERE id = 1;
COMMIT;
-- a bad key, then an UPDATE of another column of the same row
BEGIN;
UPDATE fc SET pid = 96 WHERE id = 1;
UPDATE fc SET w = 5 WHERE id = 1;
COMMIT;
-- a row inserted and then updated without touching its (bad) key
BEGIN;
INSERT INTO fc VALUES (3, 92, 0);
UPDATE fc SET w = 1 WHERE id = 3;
COMMIT;
-- a parent key changed and changed back (NO ACTION) is fine at commit
BEGIN;
UPDATE fp SET id = 3 WHERE id = 1;
UPDATE fp SET id = 1 WHERE id = 3;
COMMIT;
-- the same, checked immediately
BEGIN;
SET CONSTRAINTS ALL IMMEDIATE;
UPDATE fc SET pid = 97 WHERE id = 1;
ROLLBACK;
SELECT 'fc' AS t, * FROM fc UNION ALL SELECT 'fc_na', * FROM fc_na ORDER BY 1, 2;

--
-- 5. A self-referencing FK.
--
CREATE TABLE tree (id int PRIMARY KEY, parent int REFERENCES tree ON UPDATE CASCADE ON DELETE CASCADE, w int);
INSERT INTO tree VALUES (1, NULL, 0), (2, 1, 0), (3, 2, 0), (4, 1, 0);
UPDATE tree SET w = 1;
UPDATE tree SET id = 10 WHERE id = 1;
UPDATE tree SET id = 20 WHERE id = 2;
UPDATE tree SET parent = 4 WHERE id = 3;
UPDATE tree SET parent = 5 WHERE id = 3;
SELECT * FROM tree ORDER BY id;
DELETE FROM tree WHERE id = 10;
SELECT count(*) FROM tree;

--
-- 6. An FK across access methods, in both directions.
--
CREATE TABLE xp_o (id int PRIMARY KEY, v int) USING :other;
CREATE TABLE xc_d (id int PRIMARY KEY, pid int REFERENCES xp_o ON UPDATE CASCADE ON DELETE SET NULL, w int);
CREATE TABLE xp_d (id int PRIMARY KEY, v int);
CREATE TABLE xc_o (id int PRIMARY KEY, pid int REFERENCES xp_d ON UPDATE CASCADE ON DELETE SET NULL, w int) USING :other;
INSERT INTO xp_o VALUES (1, 0), (2, 0);
INSERT INTO xp_d VALUES (1, 0), (2, 0);
INSERT INTO xc_d VALUES (1, 1, 0), (2, 2, 0);
INSERT INTO xc_o VALUES (1, 1, 0), (2, 2, 0);
UPDATE xc_d SET w = 1;
UPDATE xc_o SET w = 1;
UPDATE xc_d SET pid = 2 WHERE id = 1;
UPDATE xc_o SET pid = 2 WHERE id = 1;
UPDATE xc_d SET pid = 3 WHERE id = 1;
UPDATE xc_o SET pid = 3 WHERE id = 1;
UPDATE xp_o SET v = 1;
UPDATE xp_d SET v = 1;
UPDATE xp_o SET id = 5 WHERE id = 2;
UPDATE xp_d SET id = 5 WHERE id = 2;
DELETE FROM xp_o WHERE id = 1;
DELETE FROM xp_d WHERE id = 1;
SELECT 'xc_d' AS t, * FROM xc_d UNION ALL SELECT 'xc_o', * FROM xc_o ORDER BY 1, 2;

--
-- 7. Partitioned tables, and 9. UPDATEs that move a row across partitions.
--
CREATE TABLE ptp (id int PRIMARY KEY, v int) PARTITION BY RANGE (id);
CREATE TABLE ptp1 PARTITION OF ptp FOR VALUES FROM (0) TO (100);
CREATE TABLE ptp2 PARTITION OF ptp FOR VALUES FROM (100) TO (200);
CREATE TABLE ptc (id int PRIMARY KEY, pid int REFERENCES ptp ON UPDATE CASCADE ON DELETE SET NULL, w int) PARTITION BY RANGE (id);
CREATE TABLE ptc1 PARTITION OF ptc FOR VALUES FROM (0) TO (100);
CREATE TABLE ptc2 PARTITION OF ptc FOR VALUES FROM (100) TO (200);
INSERT INTO ptp VALUES (1, 0), (2, 0), (150, 0);
INSERT INTO ptc VALUES (1, 1, 0), (2, 2, 0), (101, 150, 0);
UPDATE ptc SET w = 1;
UPDATE ptc SET pid = 2 WHERE id = 1;
UPDATE ptc SET pid = 3 WHERE id = 1;
UPDATE ptp SET v = 1;
UPDATE ptp SET id = 3 WHERE id = 2;
UPDATE ptp SET id = 160 WHERE id = 150;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;
-- parent row moves partition; its children follow by cascade
UPDATE ptp SET id = 120 WHERE id = 3;
-- child row moves partition, with and without an FK column change
UPDATE ptc SET id = 150 WHERE id = 1;
UPDATE ptc SET id = 50, pid = 160 WHERE id = 101;
UPDATE ptc SET id = 151, pid = 7 WHERE id = 150;
SELECT tableoid::regclass, * FROM ptp ORDER BY id;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;
DELETE FROM ptp WHERE id = 120;
SELECT tableoid::regclass, * FROM ptc ORDER BY id;

--
-- 8. MERGE on FK tables, child and parent side.
--
MERGE INTO pc USING (VALUES (1, 1, 7), (5, 2, 0), (2, NULL, 9)) s(id, pid, w)
  ON pc.id = s.id
  WHEN MATCHED AND s.pid IS NULL THEN UPDATE SET w = s.w
  WHEN MATCHED THEN UPDATE SET pid = s.pid, w = s.w
  WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.pid, s.w);
MERGE INTO pc USING (VALUES (1, 4)) s(id, pid) ON pc.id = s.id
  WHEN MATCHED THEN UPDATE SET pid = s.pid;
SELECT * FROM pc ORDER BY id;
MERGE INTO p_ca USING (VALUES (12, 13), (20, 21)) s(id, nid) ON p_ca.id = s.id
  WHEN MATCHED THEN UPDATE SET id = s.nid;
SELECT * FROM c_ca ORDER BY id;
MERGE INTO dp USING (VALUES (2)) s(id) ON dp.id = s.id
  WHEN MATCHED THEN DELETE;
SELECT 'ca' AS t, * FROM dc_ca UNION ALL SELECT 'sn', * FROM dc_sn ORDER BY 1, 2;

--
-- 10. Other AFTER-trigger users of the old/new slot: a row trigger that
-- reads OLD and NEW (immediate and deferred), transition tables, and a
-- BEFORE trigger combined with an FK.
--
CREATE TABLE tp (id int PRIMARY KEY, v int);
CREATE TABLE tc (id int PRIMARY KEY, pid int REFERENCES tp ON UPDATE CASCADE, w int);
INSERT INTO tp VALUES (1, 0), (2, 0);
INSERT INTO tc VALUES (1, 1, 0), (2, 2, 0), (3, 1, 0);
CREATE TRIGGER tc_after AFTER UPDATE ON tc
  FOR EACH ROW EXECUTE FUNCTION fk_log_row();
CREATE CONSTRAINT TRIGGER tc_deferred AFTER UPDATE ON tc
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE FUNCTION fk_log_row();
CREATE TRIGGER tc_trans AFTER UPDATE ON tc
  REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION fk_show_transition();
CREATE TRIGGER tp_trans AFTER UPDATE ON tp
  REFERENCING OLD TABLE AS oldtab NEW TABLE AS newtab
  FOR EACH STATEMENT EXECUTE FUNCTION fk_show_transition();
CREATE TRIGGER tc_before BEFORE UPDATE ON tc
  FOR EACH ROW EXECUTE FUNCTION fk_bump_w();
UPDATE tc SET w = 5 WHERE id = 1;
UPDATE tc SET pid = 2 WHERE id = 1;
UPDATE tc SET pid = 9 WHERE id = 1;
UPDATE tp SET id = 10 WHERE id = 1;
BEGIN;
UPDATE tc SET w = 0 WHERE id = 2;
UPDATE tc SET w = 1 WHERE id = 2;
UPDATE tc SET pid = 10 WHERE id = 2;
COMMIT;
-- the BEFORE trigger redirects the FK to a missing parent
UPDATE tc SET w = -1 WHERE id = 2;
SELECT * FROM tc ORDER BY id;
SELECT tbl, op, old_row, new_row FROM log ORDER BY seq;

--
-- The twins must agree on every table's contents.
--
RESET search_path;
RESET default_table_access_method;
CREATE FUNCTION pg_temp.fk_twin_diff(t text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE n bigint;
BEGIN
  EXECUTE format('SELECT count(*) FROM ((SELECT * FROM fk_heap.%1$I EXCEPT ALL SELECT * FROM fk_flux.%1$I)'
                 ' UNION ALL (SELECT * FROM fk_flux.%1$I EXCEPT ALL SELECT * FROM fk_heap.%1$I)) d', t)
    INTO n;
  RETURN n;
END $$;
SELECT c.relname, pg_temp.fk_twin_diff(c.relname) AS differing_rows
  FROM pg_class c
  WHERE c.relnamespace = 'fk_heap'::regnamespace AND c.relkind IN ('r', 'p')
    AND NOT c.relispartition
  ORDER BY 1;
SELECT c.relname, a.amname
  FROM pg_class c JOIN pg_am a ON a.oid = c.relam
  WHERE c.relnamespace = 'fk_flux'::regnamespace AND c.relkind = 'r'
  ORDER BY 1;

DROP SCHEMA fk_heap CASCADE;
DROP SCHEMA fk_flux CASCADE;
DROP FUNCTION public.fk_log_row(), public.fk_show_transition(), public.fk_bump_w();
