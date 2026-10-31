--
-- FLUX table access method: core functional + index-integrity tests.
--
-- The critical property is that
-- index scans, sequential scans, and bitmap scans agree after key-changing
-- UPDATEs, including A -> B -> A recurrences.  FLUX achieves this by doing a
-- non-in-place (new-TID) UPDATE whenever an indexed column changes, so
-- secondary indexes are maintained by the standard heap-TID path.
--

--
-- amcheck (bt_index_check) is a contrib extension, so it may be absent when
-- the core regression suite runs without EXTRA_INSTALL=contrib/amcheck (for
-- example when another test module re-runs the schedule, or on a packager's
-- "make check").  Verify indexes with it when present; otherwise skip, so the
-- expected output is identical either way.  The idxscan-vs-seqscan checks
-- below are the primary consistency assertion; amcheck is a stronger backstop.
--
CREATE FUNCTION flux_check_index(regclass) RETURNS text
LANGUAGE plpgsql AS $$
BEGIN
    IF to_regprocedure('bt_index_check(regclass,boolean)') IS NOT NULL THEN
        PERFORM bt_index_check($1, true);
    END IF;
    RETURN 'ok';
END;
$$;

CREATE TABLE flux_basic (id int, k int, v text) USING flux;
INSERT INTO flux_basic SELECT g, g, 'v' || g FROM generate_series(1, 20) g;
CREATE INDEX flux_basic_k_idx ON flux_basic (k);

-- non-key UPDATE (in place): TID and index unchanged
UPDATE flux_basic SET v = 'updated' WHERE id = 5;
SELECT id, k, v FROM flux_basic WHERE id = 5;

-- key-changing UPDATEs (out of place): old index entry dies, new one inserted
UPDATE flux_basic SET k = 105 WHERE id = 5;
UPDATE flux_basic SET k = 106 WHERE id = 6;
-- A -> B -> A recurrence on an indexed key
UPDATE flux_basic SET k = 999 WHERE id = 7;
UPDATE flux_basic SET k = 7   WHERE id = 7;

DELETE FROM flux_basic WHERE id = 20;

-- The gate: idxscan == seqscan == bitmapscan.
SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS seq_count, sum(k) AS seq_sumk FROM flux_basic;
SET enable_seqscan = off; SET enable_indexscan = on;  SET enable_bitmapscan = off;
SELECT count(*) AS idx_count, sum(k) AS idx_sumk FROM flux_basic WHERE k > -2147483648;
SELECT count(*) AS idx_k_eq7   FROM flux_basic WHERE k = 7;    -- 1, not 2
SELECT count(*) AS idx_k_eq5   FROM flux_basic WHERE k = 5;    -- 0 (moved to 105)
SELECT count(*) AS idx_k_eq105 FROM flux_basic WHERE k = 105;  -- 1
SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = on;
SELECT count(*) AS bmp_count, sum(k) AS bmp_sumk FROM flux_basic WHERE k > -2147483648;
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;

-- VACUUM then amcheck (heapallindexed): no error means the index is consistent
-- with the heap after key churn.
VACUUM flux_basic;
SELECT flux_check_index('flux_basic_k_idx'::regclass);

-- ROLLBACK restores the old value (non-key) and old key + index entry (key).
BEGIN;
UPDATE flux_basic SET v = 'should_not_persist' WHERE id = 10;
ROLLBACK;
SELECT v FROM flux_basic WHERE id = 10;

BEGIN;
UPDATE flux_basic SET k = 5000 WHERE id = 11;
ROLLBACK;
SELECT k FROM flux_basic WHERE id = 11;
SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
SELECT count(*) AS idx_k_eq11   FROM flux_basic WHERE k = 11;    -- 1
SELECT count(*) AS idx_k_eq5000 FROM flux_basic WHERE k = 5000;  -- 0
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;

-- TOAST: a >8KB value round-trips exactly and the relation gets a TOAST table.
CREATE TABLE flux_toast (id int, big text) USING flux;
INSERT INTO flux_toast VALUES (1, repeat('X', 100000));
SELECT id, length(big) AS len, (big = repeat('X', 100000)) AS exact FROM flux_toast;
SELECT reltoastrelid <> 0 AS has_toast_table FROM pg_class WHERE relname = 'flux_toast';

DROP TABLE flux_basic;
DROP TABLE flux_toast;

--
-- BUG 2 regression: an in-place UPDATE of a NON-indexed column that GROWS the
-- row on a full page must NOT error with "does not fit on page".  FLUX falls
-- back to an out-of-place (new-TID) update and rebuilds all index entries, so
-- the row survives and every index stays consistent with the heap.
--
CREATE TABLE flux_grow (id int, k int, pad text, v text) USING flux
  WITH (fillfactor = 100);
CREATE INDEX flux_grow_k_idx ON flux_grow (k);
-- Pack pages tight: many rows with a NULL v (narrowest) so the page is full
-- of committed tuples with no free space and no slack for in-place growth.
INSERT INTO flux_grow SELECT g, g, repeat('p', 200), NULL
  FROM generate_series(1, 2000) g;
VACUUM flux_grow;
-- Grow a NON-indexed column (v: NULL -> long string) on rows across full
-- pages.  Pre-fix this raised: ERROR updated flux tuple does not fit on page.
UPDATE flux_grow SET v = repeat('w', 400) WHERE id <= 2000;
-- All rows survived with the grown value.
SELECT count(*) AS grown, count(*) FILTER (WHERE v = repeat('w', 400)) AS correct
  FROM flux_grow;
-- The non-indexed grow may have moved TIDs; every index must still agree with
-- the heap (idxscan == seqscan) and amcheck must pass.
VACUUM flux_grow;
SELECT flux_check_index('flux_grow_k_idx'::regclass);
SET enable_seqscan = on;  SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) AS seq_count, sum(k) AS seq_sumk FROM flux_grow;
SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
SELECT count(*) AS idx_count, sum(k) AS idx_sumk FROM flux_grow;
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;
-- A specific indexed lookup after the TID-moving grow returns exactly one row.
SELECT id, k, length(v) AS vlen FROM flux_grow WHERE k = 1000;
DROP TABLE flux_grow;

--
-- BUG 7/8 regression: SAVEPOINT interaction with in-place UPDATE.
-- (7) Re-UPDATE inside a savepoint a row already updated by an earlier command
--     must NOT self-wait/assert.  (8) ROLLBACK TO SAVEPOINT must RESTORE the
--     pre-savepoint value of in-place updates (and undo in-savepoint deletes),
--     not vanish the row or over-revert to the original committed value.
--
CREATE TABLE flux_sp (id int PRIMARY KEY, v int) USING flux;
INSERT INTO flux_sp VALUES (1,100),(2,200),(3,300);
BEGIN;
UPDATE flux_sp SET v = 101 WHERE id = 1;   -- before savepoint
SAVEPOINT s1;
UPDATE flux_sp SET v = 999 WHERE id = 1;   -- re-update same row (bug 7)
DELETE FROM flux_sp WHERE id = 2;          -- in-savepoint delete
UPDATE flux_sp SET v = 301 WHERE id = 3;   -- in-savepoint update
ROLLBACK TO s1;                             -- must restore pre-savepoint state
-- Expect 1|101 (pre-savepoint update kept), 2|200 (delete undone), 3|300 (update undone)
SELECT id, v FROM flux_sp ORDER BY id;
COMMIT;
SELECT id, v FROM flux_sp ORDER BY id;
DROP TABLE flux_sp;

--
-- ALWAYS-IN-PLACE indexed-column UPDATE via delete-marking.
-- An UPDATE of an indexed column keeps the STABLE TID (ctid unchanged), the
-- index sees the new key (old key delete-marked), unique is respected, and
-- ROLLBACK of an in-place key update restores the old key + removes the new
-- index entry.  amcheck stays clean.
--
CREATE TABLE flux_ip (id int PRIMARY KEY, k int, v text) USING flux;
INSERT INTO flux_ip SELECT g, g, 'v'||g FROM generate_series(1,10) g;
CREATE INDEX flux_ip_k ON flux_ip (k);
CREATE UNIQUE INDEX flux_ip_uk ON flux_ip (k);
-- (a) STABLE TID: ctid identical before and after an indexed-column UPDATE.
SELECT ctid AS ip_before FROM flux_ip WHERE id = 3;
UPDATE flux_ip SET k = 333 WHERE id = 3;
SELECT ctid AS ip_after, k FROM flux_ip WHERE id = 3;   -- same ctid, k=333
-- index scan sees new key, not old; idxscan == seqscan (no duplicate rows).
SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
SELECT count(*) AS ip_k3   FROM flux_ip WHERE k = 3;    -- 0 (moved)
SELECT count(*) AS ip_k333 FROM flux_ip WHERE k = 333;  -- 1
SELECT count(*) AS ip_idx_total, sum(k) AS ip_idx_sumk FROM flux_ip WHERE k > -2147483648;
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;
SELECT count(*) AS ip_seq_total, sum(k) AS ip_seq_sumk FROM flux_ip;  -- must match idx
-- (d) UNIQUE respected: another row cannot take the new key.
INSERT INTO flux_ip VALUES (11, 11, 'x');
UPDATE flux_ip SET k = 333 WHERE id = 11;   -- ERROR: duplicate key
-- (c) ROLLBACK of an in-place key UPDATE restores old key and removes new entry.
SELECT ctid AS ip_roll_before FROM flux_ip WHERE id = 4;
BEGIN;
UPDATE flux_ip SET k = 4444 WHERE id = 4;
SELECT ctid AS ip_roll_intxn, k FROM flux_ip WHERE id = 4;   -- same ctid, k=4444
ROLLBACK;
SELECT ctid AS ip_roll_after, k FROM flux_ip WHERE id = 4;   -- same ctid, k=4 restored
SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
SELECT count(*) AS ip_roll_k4    FROM flux_ip WHERE k = 4;     -- 1 (restored)
SELECT count(*) AS ip_roll_k4444 FROM flux_ip WHERE k = 4444;  -- 0 (new entry gone)
RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;
-- Unique still works after rollback: k=4444 free to insert, k=4 taken.
INSERT INTO flux_ip VALUES (12, 4444, 'y');   -- ok
INSERT INTO flux_ip VALUES (13, 4, 'z');       -- ERROR: duplicate key (4 live)
-- (e) amcheck clean on both indexes carrying delete-marks.
SELECT flux_check_index('flux_ip_k'::regclass);
SELECT flux_check_index('flux_ip_uk'::regclass);
DROP TABLE flux_ip;

--
-- Consumer-reported regressions (undo-bug-report): the AFTER-trigger OLD image
-- and the same-transaction snapshot must match heap for an in-place AM.
--
-- BUG 1: an AFTER ROW UPDATE trigger, and RI, must see the pre-update OLD row.
CREATE TABLE flux_bug1 (id int PRIMARY KEY, v int) USING flux;
INSERT INTO flux_bug1 VALUES (1, 10);
CREATE FUNCTION flux_bug1_log() RETURNS trigger LANGUAGE plpgsql AS
  $$ BEGIN RAISE NOTICE 'OLD.v=% NEW.v=%', OLD.v, NEW.v; RETURN NULL; END $$;
CREATE TRIGGER t AFTER UPDATE ON flux_bug1 FOR EACH ROW
  EXECUTE FUNCTION flux_bug1_log();
UPDATE flux_bug1 SET v = 20;   -- expect NOTICE OLD.v=10 NEW.v=20
DROP TABLE flux_bug1;
DROP FUNCTION flux_bug1_log();

-- BUG 1 (RI): ON UPDATE RESTRICT must refuse a parent-key change that a child
-- still references; the pre-fix in-place AM saw OLD.k==NEW.k and allowed it.
CREATE TABLE flux_pr (k int PRIMARY KEY) USING flux;
INSERT INTO flux_pr VALUES (10);
CREATE TABLE flux_fr (k int REFERENCES flux_pr ON UPDATE RESTRICT);
INSERT INTO flux_fr VALUES (10);
UPDATE flux_pr SET k = 20;   -- expect ERROR: RESTRICT violation
SELECT count(*) AS flux_orphans FROM flux_fr f
  WHERE NOT EXISTS (SELECT 1 FROM flux_pr p WHERE p.k = f.k);   -- 0
DROP TABLE flux_fr; DROP TABLE flux_pr;

-- BUG 2: a snapshot taken between two in-place UPDATEs in one transaction must
-- see the first UPDATE's effect, not the last committed value.
CREATE TABLE flux_bug2 (id int PRIMARY KEY, v int) USING flux;
INSERT INTO flux_bug2 VALUES (1, 10);
BEGIN;
DECLARE c1 CURSOR FOR SELECT v FROM flux_bug2;   -- before UPDATE 1
UPDATE flux_bug2 SET v = 20;
DECLARE c2 CURSOR FOR SELECT v FROM flux_bug2;   -- between the two UPDATEs
UPDATE flux_bug2 SET v = 30;
FETCH c1;   -- 10
FETCH c2;   -- 20
SELECT v FROM flux_bug2;   -- 30
COMMIT;
DROP TABLE flux_bug2;

-- FLUX: CREATE UNIQUE INDEX must ignore rows of aborted INSERTs.
CREATE TABLE fu (id int) USING flux;
INSERT INTO fu VALUES (1);
BEGIN; INSERT INTO fu VALUES (1); ROLLBACK;
BEGIN; INSERT INTO fu VALUES (2); SAVEPOINT s; INSERT INTO fu VALUES (2);
ROLLBACK TO s; COMMIT;
CREATE UNIQUE INDEX fu_id ON fu (id);
SET enable_seqscan = off;
SELECT id FROM fu WHERE id IN (1, 2) ORDER BY id;
RESET enable_seqscan;
DROP INDEX fu_id;
-- a committed duplicate still fails
INSERT INTO fu VALUES (1);
CREATE UNIQUE INDEX fu_id ON fu (id);
DROP TABLE fu;

-- A varlena with double alignment followed by one with int alignment: the
-- index-fetch decode must not pad after the first by its own alignment.
CREATE TABLE fal (a int8range, b text) USING flux;
CREATE INDEX fal_b ON fal (b);
INSERT INTO fal VALUES ('[7,8)', 'foo');
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT a, b FROM fal WHERE b = 'foo';
RESET enable_seqscan; RESET enable_bitmapscan;
DROP TABLE fal;

DROP FUNCTION flux_check_index(regclass);

-- Ordinary VACUUM must not move a live row to another TID.  Without an index
-- FLUX's former cross-page relocation moved tail rows forward and freed their
-- slots while their writer still referenced them.
CREATE TABLE flux_vmove (id int, v int, pad text) USING flux
  WITH (autovacuum_enabled = off);
INSERT INTO flux_vmove SELECT g, 0, repeat('x', 200) FROM generate_series(1, 400) g;
DELETE FROM flux_vmove WHERE id <= 300 OR id % 3 = 0;
CREATE TEMP TABLE flux_vmove_before AS SELECT id, ctid AS c FROM flux_vmove;
VACUUM flux_vmove;
VACUUM flux_vmove;
SELECT count(*) AS moved FROM flux_vmove JOIN flux_vmove_before b USING (id)
  WHERE flux_vmove.ctid <> b.c;   -- 0
SELECT count(*) AS n FROM flux_vmove;   -- 67
DROP TABLE flux_vmove, flux_vmove_before;

-- VACUUM keeps an empty tail when told to (VACUUM (TRUNCATE false), the
-- vacuum_truncate reloption) and when it is too short to be worth the lock:
-- under 1000 pages and 1/16 of the table.  Heap alongside as the control.
CREATE TEMP TABLE flux_tk_h (id int, pad text) USING heap;
CREATE TEMP TABLE flux_tk (id int, pad text) USING flux;
INSERT INTO flux_tk_h SELECT g, repeat('t', 200) FROM generate_series(1, 20000) g;
INSERT INTO flux_tk SELECT g, repeat('t', 200) FROM generate_series(1, 20000) g;
CREATE TEMP TABLE flux_tk_sz AS
  SELECT pg_relation_size('flux_tk_h') AS h, pg_relation_size('flux_tk') AS r;
DELETE FROM flux_tk_h WHERE id > 19900;
DELETE FROM flux_tk WHERE id > 19900;
VACUUM flux_tk_h;
VACUUM flux_tk;
SELECT pg_relation_size('flux_tk_h') = h AS heap_kept,
       pg_relation_size('flux_tk') = r AS flux_kept FROM flux_tk_sz;
DELETE FROM flux_tk_h WHERE id > 1;
DELETE FROM flux_tk WHERE id > 1;
VACUUM (TRUNCATE false) flux_tk_h;
VACUUM (TRUNCATE false) flux_tk;
SELECT pg_relation_size('flux_tk_h') = h AS heap_kept,
       pg_relation_size('flux_tk') = r AS flux_kept FROM flux_tk_sz;
ALTER TABLE flux_tk_h SET (vacuum_truncate = false);
ALTER TABLE flux_tk SET (vacuum_truncate = false);
VACUUM flux_tk_h;
VACUUM flux_tk;
SELECT pg_relation_size('flux_tk_h') = h AS heap_kept,
       pg_relation_size('flux_tk') = r AS flux_kept FROM flux_tk_sz;
ALTER TABLE flux_tk_h RESET (vacuum_truncate);
ALTER TABLE flux_tk RESET (vacuum_truncate);
VACUUM flux_tk_h;
VACUUM flux_tk;
SELECT pg_relation_size('flux_tk_h') = 8192 AS heap_truncated,
       pg_relation_size('flux_tk') = 8192 AS flux_truncated;
DROP TABLE flux_tk_h, flux_tk, flux_tk_sz;

-- VACUUM (DISABLE_PAGE_SKIPPING) still truncates an empty tail, as heap does.
CREATE TEMP TABLE flux_dps_h (id int, pad text) USING heap;
CREATE TEMP TABLE flux_dps (id int, pad text) USING flux;
INSERT INTO flux_dps_h SELECT g, repeat('t', 200) FROM generate_series(1, 20000) g;
INSERT INTO flux_dps SELECT g, repeat('t', 200) FROM generate_series(1, 20000) g;
DELETE FROM flux_dps_h WHERE id > 1;
DELETE FROM flux_dps WHERE id > 1;
VACUUM (DISABLE_PAGE_SKIPPING) flux_dps_h;
VACUUM (DISABLE_PAGE_SKIPPING) flux_dps;
SELECT pg_relation_size('flux_dps_h') = 8192 AS heap_truncated,
       pg_relation_size('flux_dps') = 8192 AS flux_truncated;
DROP TABLE flux_dps_h, flux_dps;

-- Heap control, then the same operations with FLUX.
CREATE TABLE scan_semantics (a int) USING heap;
INSERT INTO scan_semantics SELECT generate_series(1,5);
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
FETCH 3 FROM c;
FETCH BACKWARD 1 FROM c;
FETCH LAST FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
MOVE BACKWARD ALL FROM c;
FETCH NEXT FROM c;
COMMIT;
SET enable_seqscan = off;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics
 WHERE ctid >= '(0,2)' AND ctid <= '(0,4)';
FETCH LAST FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH NEXT FROM c;
COMMIT;
RESET enable_seqscan;
DROP TABLE scan_semantics;

CREATE TABLE scan_semantics (a int, pad text) USING heap;
INSERT INTO scan_semantics SELECT g, repeat('x', 100) FROM generate_series(1,1000) g;
EXPLAIN (COSTS OFF) SELECT a FROM scan_semantics;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
MOVE 500 FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
FETCH LAST FROM c;
FETCH NEXT FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
COMMIT;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT a FROM scan_semantics
 WHERE ctid >= '(1,1)' AND ctid < '(2,1)';
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT true AS found FROM scan_semantics
 WHERE ctid >= '(1,1)' AND ctid < '(2,1)';
FETCH FIRST FROM c;
FETCH LAST FROM c;
FETCH NEXT FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
COMMIT;
RESET enable_seqscan;
TRUNCATE scan_semantics;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
FETCH LAST FROM c;
FETCH FIRST FROM c;
COMMIT;
DROP TABLE scan_semantics;

CREATE TABLE scan_semantics (a int) USING flux;
INSERT INTO scan_semantics SELECT generate_series(1,5);
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
FETCH 3 FROM c;
FETCH BACKWARD 1 FROM c;
FETCH LAST FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
MOVE BACKWARD ALL FROM c;
FETCH NEXT FROM c;
COMMIT;
SET enable_seqscan = off;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics
 WHERE ctid >= '(0,2)' AND ctid <= '(0,4)';
FETCH LAST FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH NEXT FROM c;
COMMIT;
RESET enable_seqscan;
DROP TABLE scan_semantics;

CREATE TABLE scan_semantics (a int, pad text) USING flux;
INSERT INTO scan_semantics SELECT g, repeat('x', 100) FROM generate_series(1,1000) g;
EXPLAIN (COSTS OFF) SELECT a FROM scan_semantics;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
MOVE 500 FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
FETCH LAST FROM c;
FETCH NEXT FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
COMMIT;
SET enable_seqscan = off;
EXPLAIN (COSTS OFF) SELECT a FROM scan_semantics
 WHERE ctid >= '(1,1)' AND ctid < '(2,1)';
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT true AS found FROM scan_semantics
 WHERE ctid >= '(1,1)' AND ctid < '(2,1)';
FETCH FIRST FROM c;
FETCH LAST FROM c;
FETCH NEXT FROM c;
FETCH PRIOR FROM c;
FETCH FIRST FROM c;
FETCH PRIOR FROM c;
FETCH NEXT FROM c;
COMMIT;
RESET enable_seqscan;
TRUNCATE scan_semantics;
BEGIN;
DECLARE c SCROLL CURSOR FOR SELECT a FROM scan_semantics;
FETCH LAST FROM c;
FETCH FIRST FROM c;
COMMIT;
DROP TABLE scan_semantics;

-- Heap control, then the same operations with FLUX.
CREATE TABLE scan_semantics () USING heap;
INSERT INTO scan_semantics DEFAULT VALUES;
ALTER TABLE scan_semantics ADD COLUMN a int DEFAULT 1;
CREATE INDEX ON scan_semantics(a);
UPDATE scan_semantics SET a = NULL;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM scan_semantics WHERE a IS NULL;
RESET enable_indexscan;
SET enable_seqscan = off;
SELECT count(*) FROM scan_semantics WHERE a IS NULL;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE scan_semantics;

CREATE TABLE scan_semantics () USING flux;
INSERT INTO scan_semantics DEFAULT VALUES;
ALTER TABLE scan_semantics ADD COLUMN a int DEFAULT 1;
CREATE INDEX ON scan_semantics(a);
UPDATE scan_semantics SET a = NULL;
SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM scan_semantics WHERE a IS NULL;
RESET enable_indexscan;
SET enable_seqscan = off;
SELECT count(*) FROM scan_semantics WHERE a IS NULL;
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE scan_semantics;

-- Heap control, then the same operations with FLUX.
CREATE TABLE scan_semantics (pad text, a int) USING heap;
INSERT INTO scan_semantics SELECT repeat('x',100), generate_series(1,1000);
CREATE INDEX ON scan_semantics(a);
VACUUM ANALYZE scan_semantics;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE AGGREGATE scan_count (*) (sfunc = int8inc, stype = int8,
 initcond = '0', parallel = safe);
SELECT scan_count(*) FROM scan_semantics;
SET debug_parallel_query = on;
SELECT scan_count(*) FROM scan_semantics;
SELECT count(*) FROM scan_semantics;
RESET debug_parallel_query;
DROP AGGREGATE scan_count(*);
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE scan_semantics;

CREATE TABLE scan_semantics (pad text, a int) USING flux;
INSERT INTO scan_semantics SELECT repeat('x',100), generate_series(1,1000);
CREATE INDEX ON scan_semantics(a);
VACUUM ANALYZE scan_semantics;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
CREATE AGGREGATE scan_count (*) (sfunc = int8inc, stype = int8,
 initcond = '0', parallel = safe);
SELECT scan_count(*) FROM scan_semantics;
SET debug_parallel_query = on;
SELECT scan_count(*) FROM scan_semantics;
SELECT count(*) FROM scan_semantics;
RESET debug_parallel_query;
DROP AGGREGATE scan_count(*);
RESET enable_seqscan;
RESET enable_bitmapscan;
DROP TABLE scan_semantics;

-- Heap control, then the same operations with FLUX.
CREATE TABLE self_direct (id int, v int) USING heap;
INSERT INTO self_direct VALUES (1,0);
CREATE FUNCTION self_direct_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF pg_trigger_depth() = 1 THEN
  UPDATE self_direct SET v = v + 1 WHERE id = OLD.id;
 END IF;
 RETURN OLD;
END $$;
CREATE TRIGGER self_direct_tr BEFORE DELETE OR UPDATE ON self_direct
 FOR EACH ROW EXECUTE FUNCTION self_direct_fn();
DELETE FROM self_direct;
SELECT * FROM self_direct;
UPDATE self_direct SET v = 10;
SELECT * FROM self_direct;
DROP TABLE self_direct;
DROP FUNCTION self_direct_fn();

-- Parent-first cascade succeeds; child-first cascade self-modifies.
create temp table self_ref_trigger (
    id int primary key,
    parent int references self_ref_trigger,
    data text,
    nchildren int not null default 0
) USING heap;

create function self_ref_trigger_ins_func()
  returns trigger language plpgsql as
$$
begin
  if new.parent is not null then
    update self_ref_trigger set nchildren = nchildren + 1
      where id = new.parent;
  end if;
  return new;
end;
$$;
create trigger self_ref_trigger_ins_trig before insert on self_ref_trigger
  for each row execute procedure self_ref_trigger_ins_func();

create function self_ref_trigger_del_func()
  returns trigger language plpgsql as
$$
begin
  if old.parent is not null then
    update self_ref_trigger set nchildren = nchildren - 1
      where id = old.parent;
  end if;
  return old;
end;
$$;
create trigger self_ref_trigger_del_trig before delete on self_ref_trigger
  for each row execute procedure self_ref_trigger_del_func();

insert into self_ref_trigger values (1, null, 'root');
insert into self_ref_trigger values (2, 1, 'root child A');
insert into self_ref_trigger values (3, 1, 'root child B');
insert into self_ref_trigger values (4, 2, 'grandchild 1');
insert into self_ref_trigger values (5, 3, 'grandchild 2');

update self_ref_trigger set data = 'root!' where id = 1;

select * from self_ref_trigger ORDER BY id;

ALTER TABLE self_ref_trigger DISABLE TRIGGER ALL;
TRUNCATE self_ref_trigger;
INSERT INTO self_ref_trigger SELECT * FROM (VALUES
 (1,null::int,'root!',2),(2,1,'root child A',1),(3,1,'root child B',1),
 (4,2,'grandchild 1',0),(5,3,'grandchild 2',0)) v(id,parent,data,nchildren)
 ORDER BY id ASC;
ALTER TABLE self_ref_trigger ENABLE TRIGGER ALL;
SELECT id FROM self_ref_trigger;
delete from self_ref_trigger;

select * from self_ref_trigger ORDER BY id;

drop table self_ref_trigger;
drop function self_ref_trigger_ins_func();
drop function self_ref_trigger_del_func();
create temp table self_ref_trigger (
    id int primary key,
    parent int references self_ref_trigger,
    data text,
    nchildren int not null default 0
) USING heap;

create function self_ref_trigger_ins_func()
  returns trigger language plpgsql as
$$
begin
  if new.parent is not null then
    update self_ref_trigger set nchildren = nchildren + 1
      where id = new.parent;
  end if;
  return new;
end;
$$;
create trigger self_ref_trigger_ins_trig before insert on self_ref_trigger
  for each row execute procedure self_ref_trigger_ins_func();

create function self_ref_trigger_del_func()
  returns trigger language plpgsql as
$$
begin
  if old.parent is not null then
    update self_ref_trigger set nchildren = nchildren - 1
      where id = old.parent;
  end if;
  return old;
end;
$$;
create trigger self_ref_trigger_del_trig before delete on self_ref_trigger
  for each row execute procedure self_ref_trigger_del_func();

insert into self_ref_trigger values (1, null, 'root');
insert into self_ref_trigger values (2, 1, 'root child A');
insert into self_ref_trigger values (3, 1, 'root child B');
insert into self_ref_trigger values (4, 2, 'grandchild 1');
insert into self_ref_trigger values (5, 3, 'grandchild 2');

update self_ref_trigger set data = 'root!' where id = 1;

select * from self_ref_trigger ORDER BY id;

ALTER TABLE self_ref_trigger DISABLE TRIGGER ALL;
TRUNCATE self_ref_trigger;
INSERT INTO self_ref_trigger SELECT * FROM (VALUES
 (1,null::int,'root!',2),(2,1,'root child A',1),(3,1,'root child B',1),
 (4,2,'grandchild 1',0),(5,3,'grandchild 2',0)) v(id,parent,data,nchildren)
 ORDER BY id DESC;
ALTER TABLE self_ref_trigger ENABLE TRIGGER ALL;
SELECT id FROM self_ref_trigger;
delete from self_ref_trigger;

select * from self_ref_trigger ORDER BY id;

drop table self_ref_trigger;
drop function self_ref_trigger_ins_func();
drop function self_ref_trigger_del_func();

CREATE TABLE self_direct (id int, v int) USING flux;
INSERT INTO self_direct VALUES (1,0);
CREATE FUNCTION self_direct_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
 IF pg_trigger_depth() = 1 THEN
  UPDATE self_direct SET v = v + 1 WHERE id = OLD.id;
 END IF;
 RETURN OLD;
END $$;
CREATE TRIGGER self_direct_tr BEFORE DELETE OR UPDATE ON self_direct
 FOR EACH ROW EXECUTE FUNCTION self_direct_fn();
DELETE FROM self_direct;
SELECT * FROM self_direct;
UPDATE self_direct SET v = 10;
SELECT * FROM self_direct;
DROP TABLE self_direct;
DROP FUNCTION self_direct_fn();

-- Parent-first cascade succeeds; child-first cascade self-modifies.
create temp table self_ref_trigger (
    id int primary key,
    parent int references self_ref_trigger,
    data text,
    nchildren int not null default 0
) USING flux;

create function self_ref_trigger_ins_func()
  returns trigger language plpgsql as
$$
begin
  if new.parent is not null then
    update self_ref_trigger set nchildren = nchildren + 1
      where id = new.parent;
  end if;
  return new;
end;
$$;
create trigger self_ref_trigger_ins_trig before insert on self_ref_trigger
  for each row execute procedure self_ref_trigger_ins_func();

create function self_ref_trigger_del_func()
  returns trigger language plpgsql as
$$
begin
  if old.parent is not null then
    update self_ref_trigger set nchildren = nchildren - 1
      where id = old.parent;
  end if;
  return old;
end;
$$;
create trigger self_ref_trigger_del_trig before delete on self_ref_trigger
  for each row execute procedure self_ref_trigger_del_func();

insert into self_ref_trigger values (1, null, 'root');
insert into self_ref_trigger values (2, 1, 'root child A');
insert into self_ref_trigger values (3, 1, 'root child B');
insert into self_ref_trigger values (4, 2, 'grandchild 1');
insert into self_ref_trigger values (5, 3, 'grandchild 2');

update self_ref_trigger set data = 'root!' where id = 1;

select * from self_ref_trigger ORDER BY id;

ALTER TABLE self_ref_trigger DISABLE TRIGGER ALL;
TRUNCATE self_ref_trigger;
INSERT INTO self_ref_trigger SELECT * FROM (VALUES
 (1,null::int,'root!',2),(2,1,'root child A',1),(3,1,'root child B',1),
 (4,2,'grandchild 1',0),(5,3,'grandchild 2',0)) v(id,parent,data,nchildren)
 ORDER BY id ASC;
ALTER TABLE self_ref_trigger ENABLE TRIGGER ALL;
SELECT id FROM self_ref_trigger;
delete from self_ref_trigger;

select * from self_ref_trigger ORDER BY id;

drop table self_ref_trigger;
drop function self_ref_trigger_ins_func();
drop function self_ref_trigger_del_func();
create temp table self_ref_trigger (
    id int primary key,
    parent int references self_ref_trigger,
    data text,
    nchildren int not null default 0
) USING flux;

create function self_ref_trigger_ins_func()
  returns trigger language plpgsql as
$$
begin
  if new.parent is not null then
    update self_ref_trigger set nchildren = nchildren + 1
      where id = new.parent;
  end if;
  return new;
end;
$$;
create trigger self_ref_trigger_ins_trig before insert on self_ref_trigger
  for each row execute procedure self_ref_trigger_ins_func();

create function self_ref_trigger_del_func()
  returns trigger language plpgsql as
$$
begin
  if old.parent is not null then
    update self_ref_trigger set nchildren = nchildren - 1
      where id = old.parent;
  end if;
  return old;
end;
$$;
create trigger self_ref_trigger_del_trig before delete on self_ref_trigger
  for each row execute procedure self_ref_trigger_del_func();

insert into self_ref_trigger values (1, null, 'root');
insert into self_ref_trigger values (2, 1, 'root child A');
insert into self_ref_trigger values (3, 1, 'root child B');
insert into self_ref_trigger values (4, 2, 'grandchild 1');
insert into self_ref_trigger values (5, 3, 'grandchild 2');

update self_ref_trigger set data = 'root!' where id = 1;

select * from self_ref_trigger ORDER BY id;

ALTER TABLE self_ref_trigger DISABLE TRIGGER ALL;
TRUNCATE self_ref_trigger;
INSERT INTO self_ref_trigger SELECT * FROM (VALUES
 (1,null::int,'root!',2),(2,1,'root child A',1),(3,1,'root child B',1),
 (4,2,'grandchild 1',0),(5,3,'grandchild 2',0)) v(id,parent,data,nchildren)
 ORDER BY id DESC;
ALTER TABLE self_ref_trigger ENABLE TRIGGER ALL;
SELECT id FROM self_ref_trigger;
delete from self_ref_trigger;

select * from self_ref_trigger ORDER BY id;

drop table self_ref_trigger;
drop function self_ref_trigger_ins_func();
drop function self_ref_trigger_del_func();
