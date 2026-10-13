# FLUX: a row lock that waits on, or meets, a committed concurrent update must
# report it exactly as heap does.
#
# FLUX updates rows in place at a stable TID, so the latest version of a row
# is always at the TID the caller holds.  table_tuple_lock() must still tell
# its callers that the version their snapshot saw was superseded:
#  - READ COMMITTED, TUPLE_LOCK_FLAG_FIND_LAST_VERSION: TM_Ok with
#    tmfd->traversed = true, so nodeLockRows, GetTupleForTrigger and
#    ri_LockPKTuple re-check the latest version (EvalPlanQual, FK key recheck);
#  - REPEATABLE READ: TM_Updated, so the caller raises "could not serialize
#    access due to concurrent update";
#  - ON CONFLICT (flags 0) under READ COMMITTED: TM_Ok on the latest version,
#    because heap's retry would find and lock that same row.
# A FOR KEY SHARE lock is compatible with a non-key UPDATE and, as in heap,
# neither waits nor reports it.
#
# The expected output is heap's: heap_lock_recheck.spec is this spec with
# USING heap, and both share the same expected .out.

setup
{
  CREATE TABLE lr (id int PRIMARY KEY, v int, w int) USING flux;
  INSERT INTO lr VALUES (1, 1, 0), (2, 1, 0);
  CREATE TABLE lrp (id int PRIMARY KEY, note int) USING flux;
  INSERT INTO lrp VALUES (1, 0);
  CREATE TABLE lrc (id int, pid int REFERENCES lrp) USING flux;
  CREATE TABLE lrt (id int PRIMARY KEY, v int, w int) USING flux;
  INSERT INTO lrt VALUES (1, 1, 0);
  CREATE FUNCTION lrt_bu() RETURNS trigger LANGUAGE plpgsql AS
    $$ BEGIN RAISE NOTICE 'lrt_bu: OLD.v=% NEW.v=%', OLD.v, NEW.v; RETURN NEW; END $$;
  CREATE TRIGGER lrt_bu BEFORE UPDATE ON lrt FOR EACH ROW EXECUTE FUNCTION lrt_bu();
  -- A BEFORE UPDATE OR DELETE trigger that modifies NEW.
  CREATE TABLE lrn (id int PRIMARY KEY, v int, w int) USING flux;
  INSERT INTO lrn VALUES (1, 1, 0);
  CREATE FUNCTION lrn_bu() RETURNS trigger LANGUAGE plpgsql AS
    $$ BEGIN
      RAISE NOTICE 'lrn_bu %: OLD=% NEW=%', TG_OP, OLD, NEW;
      IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
      NEW.w := NEW.w + 100;
      RETURN NEW;
    END $$;
  CREATE TRIGGER lrn_bu BEFORE UPDATE OR DELETE ON lrn FOR EACH ROW EXECUTE FUNCTION lrn_bu();
  -- Partitioned by v, for cross-partition UPDATE; lptt has a trigger.
  CREATE TABLE lpt (id int, v int, w int) PARTITION BY LIST (v);
  CREATE TABLE lpt1 PARTITION OF lpt FOR VALUES IN (1) USING flux;
  CREATE TABLE lpt2 PARTITION OF lpt FOR VALUES IN (2) USING flux;
  INSERT INTO lpt VALUES (1, 1, 0);
  CREATE TABLE lptt (id int, v int, w int) PARTITION BY LIST (v);
  CREATE TABLE lptt1 PARTITION OF lptt FOR VALUES IN (1) USING flux;
  CREATE TABLE lptt2 PARTITION OF lptt FOR VALUES IN (2) USING flux;
  INSERT INTO lptt VALUES (1, 1, 0);
  CREATE FUNCTION lr_note() RETURNS trigger LANGUAGE plpgsql AS
    $$ BEGIN
      RAISE NOTICE '% % %: OLD=% NEW=%', TG_TABLE_NAME, TG_WHEN, TG_OP, OLD, NEW;
      IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
      RETURN NEW;
    END $$;
  CREATE TRIGGER lr_note BEFORE UPDATE OR DELETE ON lptt FOR EACH ROW EXECUTE FUNCTION lr_note();
  -- FK with ON DELETE CASCADE: the RI action deletes a child row that a
  -- concurrent transaction has updated.
  CREATE TABLE lrq (id int PRIMARY KEY, note int) USING flux;
  INSERT INTO lrq VALUES (1, 0);
  CREATE TABLE lrqc (id int PRIMARY KEY, pid int REFERENCES lrq ON DELETE CASCADE, w int) USING flux;
  INSERT INTO lrqc VALUES (1, 1, 0);
  CREATE TABLE lrup (id int PRIMARY KEY) USING flux;
  INSERT INTO lrup VALUES (1);
  CREATE TABLE lruc (pid int REFERENCES lrup ON UPDATE CASCADE) USING flux;
  CREATE TABLE lrrp (id int PRIMARY KEY) USING flux;
  INSERT INTO lrrp VALUES (1);
  CREATE TABLE lrrc (pid int REFERENCES lrrp ON UPDATE RESTRICT) USING flux;
  CREATE TABLE lrmp (a int, b int, PRIMARY KEY (a, b)) USING flux;
  INSERT INTO lrmp VALUES (1, 1);
  CREATE TABLE lrmc (a int, b int, FOREIGN KEY (a, b) REFERENCES lrmp) USING flux;
  CREATE TABLE lrvp (id int, note int) USING flux;
  CREATE UNIQUE INDEX lrvp_unique ON lrvp (id);
  INSERT INTO lrvp VALUES (1, 0);
  CREATE TABLE lrvc (pid int REFERENCES lrvp (id)) USING flux;
}

teardown
{
  DROP TABLE lruc, lrup, lrrc, lrrp, lrmc, lrmp, lrvc, lrvp;
  DROP TABLE lr, lrc, lrp, lrt, lrn, lpt, lptt, lrqc, lrq;
  DROP FUNCTION lrt_bu(), lrn_bu(), lr_note();
}

session s1
step s1b	{ BEGIN; }
step s1sp { SAVEPOINT key_update; }
step s1rb { ROLLBACK TO key_update; }
step s1uk { UPDATE lrup SET id = 2 WHERE id = 1; }
step s1rk { UPDATE lrrp SET id = 2 WHERE id = 1; }
step s1mk { UPDATE lrmp SET b = 2 WHERE a = 1 AND b = 1; }
step s1vk { UPDATE lrvp SET id = 2 WHERE id = 1; }
step s1vn { UPDATE lrvp SET note = note + 1 WHERE id = 1; }
step s1u	{ UPDATE lr SET v = 2 WHERE id = 1; }
step s1k	{ UPDATE lr SET id = 10 WHERE id = 1; }
step s1d	{ DELETE FROM lr WHERE id = 1; }
step s1pk	{ UPDATE lrp SET id = 2 WHERE id = 1; }
step s1pfu	{ SELECT id FROM lrp WHERE id = 1 FOR UPDATE; }
step s1pn	{ UPDATE lrp SET note = 1 WHERE id = 1; }
step s1tu	{ UPDATE lrt SET v = 2 WHERE id = 1; }
step s1lfu	{ SELECT * FROM lr WHERE id = 1 FOR UPDATE; }
step s1tfu	{ SELECT * FROM lrt WHERE id = 1 FOR UPDATE; }
step s1tk	{ UPDATE lrt SET id = 10 WHERE id = 1; }
step s1tu2	{ UPDATE lrt SET v = v + 1, w = w + 10 WHERE id = 1; }
step s1nu	{ UPDATE lrn SET v = 2 WHERE id = 1; }
step s1pu	{ UPDATE lpt SET w = 5 WHERE id = 1; }
step s1ptu	{ UPDATE lptt SET w = 5 WHERE id = 1; }
# session_replication_role = replica skips lrqc's own FK check trigger, which
# FLUX cannot run yet on UPDATE (it reads xmin from the old row's slot).
step s1qcu	{ SET LOCAL session_replication_role = replica; UPDATE lrqc SET w = 5 WHERE id = 1; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }

session s2
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2a { ROLLBACK; }
step s2uci { INSERT INTO lruc VALUES (1); }
step s2rci { INSERT INTO lrrc VALUES (1); }
step s2mci { INSERT INTO lrmc VALUES (1, 1); }
step s2vci { INSERT INTO lrvc VALUES (1); }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2snap	{ SELECT count(*) FROM lr; }
step s2fu	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR UPDATE; }
step s2fnk	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR NO KEY UPDATE; }
step s2fs	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR SHARE; }
step s2fks	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR KEY SHARE; }
step s2sl	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR UPDATE SKIP LOCKED; }
step s2nw	{ SELECT * FROM lr WHERE v = 1 ORDER BY id FOR UPDATE NOWAIT; }
step s2upd	{ UPDATE lr SET w = w + 1 WHERE v = 1 RETURNING *; }
step s2ci	{ INSERT INTO lrc VALUES (1, 1); }
step s2tu	{ UPDATE lrt SET w = w + 1 WHERE v = 1 RETURNING *; }
step s2oc	{ INSERT INTO lr VALUES (1, 1, 0) ON CONFLICT (id) DO UPDATE SET w = lr.w + 1 RETURNING *; }
# The same locked row in several output rows: each is re-checked.
step s2jfu	{ SELECT lr.* FROM lr JOIN (VALUES (1), (1), (2)) s(id) USING (id) WHERE lr.v = 1 ORDER BY lr.id FOR UPDATE OF lr; }
# MERGE with a BEFORE UPDATE trigger: GetTupleForTrigger reports the
# concurrent update, MERGE re-checks the join and either drops the row or
# updates the latest version, and must not loop.
step s2mq	{ MERGE INTO lrt USING (VALUES (1)) s(id) ON lrt.id = s.id AND lrt.v = 1 WHEN MATCHED THEN UPDATE SET w = lrt.w + 1; }
step s2mu	{ MERGE INTO lrt USING (VALUES (1)) s(id) ON lrt.id = s.id WHEN MATCHED THEN UPDATE SET w = lrt.w + 1; }
# Parent-row locks after several updates (FOR KEY SHARE must see a key change
# by any of them, not only the latest).  A cursor fixes the READ COMMITTED
# snapshot before the updates.
step s2pks	{ SELECT * FROM lrp WHERE id = 1 FOR KEY SHARE; }
step s2pnk	{ SELECT * FROM lrp WHERE id = 1 FOR NO KEY UPDATE; }
step s2dks	{ DECLARE c CURSOR FOR SELECT * FROM lrp WHERE id = 1 FOR KEY SHARE; }
step s2dka	{ DECLARE c CURSOR FOR SELECT * FROM lrp FOR KEY SHARE; }
step s2dnk	{ DECLARE c CURSOR FOR SELECT * FROM lrp FOR NO KEY UPDATE; }
step s2fc	{ FETCH ALL FROM c; }
step s2pks101	{ SELECT * FROM lrp WHERE id = 101 FOR KEY SHARE; }
step s2ci101	{ INSERT INTO lrc VALUES (2, 101); }
# ON CONFLICT DO UPDATE with a BEFORE UPDATE trigger: the trigger locks the
# row ON CONFLICT already locked.
step s2toc	{ INSERT INTO lrt VALUES (1, 1, 0) ON CONFLICT (id) DO UPDATE SET w = lrt.w + 1 RETURNING *; }
step s2noc	{ INSERT INTO lrn VALUES (1, 1, 0) ON CONFLICT (id) DO UPDATE SET w = lrn.w + 1 RETURNING *; }
# MERGE without a trigger (lr) and with one that modifies NEW (lrn).
step s2mlu	{ MERGE INTO lr USING (VALUES (1)) s(id) ON lr.id = s.id WHEN MATCHED THEN UPDATE SET w = lr.w + 1 RETURNING merge_action(), lr.*; }
step s2mlq	{ MERGE INTO lr USING (VALUES (1)) s(id) ON lr.id = s.id AND lr.v = 1 WHEN MATCHED THEN UPDATE SET w = lr.w + 1 RETURNING merge_action(), lr.*; }
step s2mld	{ MERGE INTO lr USING (VALUES (1)) s(id) ON lr.id = s.id WHEN MATCHED THEN DELETE RETURNING merge_action(), lr.*; }
step s2mnu	{ MERGE INTO lrn USING (VALUES (1)) s(id) ON lrn.id = s.id WHEN MATCHED THEN UPDATE SET w = lrn.w + 1 RETURNING merge_action(), lrn.*; }
step s2mnq	{ MERGE INTO lrn USING (VALUES (1)) s(id) ON lrn.id = s.id AND lrn.v = 1 WHEN MATCHED THEN UPDATE SET w = lrn.w + 1 RETURNING merge_action(), lrn.*; }
step s2mnd	{ MERGE INTO lrn USING (VALUES (1)) s(id) ON lrn.id = s.id WHEN MATCHED THEN DELETE RETURNING merge_action(), lrn.*; }
# Cross-partition UPDATE (ExecCrossPartitionUpdate), and MERGE moving the row.
step s2pm	{ UPDATE lpt SET v = 2 WHERE id = 1 RETURNING tableoid::regclass, *; }
step s2pmq	{ UPDATE lpt SET v = 2 WHERE id = 1 AND w = 0 RETURNING tableoid::regclass, *; }
step s2ptm	{ UPDATE lptt SET v = 2 WHERE id = 1 RETURNING tableoid::regclass, *; }
step s2ptmq	{ UPDATE lptt SET v = 2 WHERE id = 1 AND w = 0 RETURNING tableoid::regclass, *; }
step s2mpm	{ MERGE INTO lpt USING (VALUES (1)) s(id) ON lpt.id = s.id WHEN MATCHED THEN UPDATE SET v = 2 RETURNING merge_action(), lpt.*; }
step s2mptm	{ MERGE INTO lptt USING (VALUES (1)) s(id) ON lptt.id = s.id WHEN MATCHED THEN UPDATE SET v = 2 RETURNING merge_action(), lptt.*; }
# RI cascade action on a child row updated concurrently.
step s2qd	{ DELETE FROM lrq WHERE id = 1; }
step s2c	{ COMMIT; }

session s3
step s3orphans { SELECT count(*) AS orphans FROM lrc c WHERE NOT EXISTS (SELECT FROM lrp p WHERE p.id = c.pid); }
step s3u { SELECT * FROM lrup; SELECT * FROM lruc; SELECT count(*) AS orphans FROM lruc c WHERE NOT EXISTS (SELECT FROM lrup p WHERE p.id = c.pid); }
step s3restrict { SELECT * FROM lrrp; SELECT * FROM lrrc; SELECT count(*) AS orphans FROM lrrc c WHERE NOT EXISTS (SELECT FROM lrrp p WHERE p.id = c.pid); }
step s3multi { SELECT * FROM lrmp; SELECT * FROM lrmc; SELECT count(*) AS orphans FROM lrmc c WHERE NOT EXISTS (SELECT FROM lrmp p WHERE p.a = c.a AND p.b = c.b); }
step s3unique { SELECT * FROM lrvp; SELECT * FROM lrvc; SELECT count(*) AS orphans FROM lrvc c WHERE NOT EXISTS (SELECT FROM lrvp p WHERE p.id = c.pid); }
step s3r	{ SELECT * FROM lr ORDER BY id; }
step s3c	{ SELECT * FROM lrc ORDER BY id; }
step s3t	{ SELECT * FROM lrt ORDER BY id; }
step s3p	{ SELECT * FROM lrp ORDER BY id; }
step s3n	{ SELECT * FROM lrn ORDER BY id; }
step s3pt	{ SELECT tableoid::regclass, * FROM lpt UNION ALL SELECT tableoid::regclass, * FROM lptt ORDER BY 1; }
step s3q	{ SELECT * FROM lrq ORDER BY id; SELECT * FROM lrqc ORDER BY id; }

# Further parent updates, each committed on its own (autocommit) or held open.
# lrp starts as (1, 0).
session s4
step s4vn { UPDATE lrvp SET note = note + 1 WHERE id = 1; }
step s4pkk	{ UPDATE lrp SET id = id + 100, note = note + 1 WHERE id = 1; }
step s4pkb	{ UPDATE lrp SET id = id - 100, note = note + 1 WHERE id = 101; }
step s4pn101	{ UPDATE lrp SET note = note + 10 WHERE id = 101; }
step s4pn1	{ UPDATE lrp SET note = note + 10 WHERE id = 1; }

session s5
step s5b	{ BEGIN; }
step s5pkk	{ UPDATE lrp SET id = id + 100, note = note + 1 WHERE id = 1; }
step s5pn101	{ UPDATE lrp SET note = note + 100 WHERE id = 101; }
step s5pn1	{ UPDATE lrp SET note = note + 100 WHERE id = 1; }
step s5c	{ COMMIT; }
step s5a	{ ROLLBACK; }

# READ COMMITTED, each lock strength that conflicts with the UPDATE waits for
# it, then re-checks v = 1 against the committed version: row 1 drops out.
permutation s1b s1u s2rc s2fu s1c s2c s3r
permutation s1b s1u s2rc s2fnk s1c s2c s3r
permutation s1b s1u s2rc s2fs s1c s2c s3r
# FOR KEY SHARE does not conflict with a non-key UPDATE: no wait, no recheck.
permutation s1b s1u s2rc s2fks s1c s2c s3r
# FOR KEY SHARE waits for a key UPDATE.
permutation s1b s1k s2rc s2fks s1c s2c s3r
# SKIP LOCKED skips the row being updated; NOWAIT fails at once.
permutation s1b s1u s2rc s2sl s1c s2c s3r
permutation s1b s1u s2rc s2nw s1c s2c s3r
# UPDATE ... WHERE v = 1 re-checks through EvalPlanQual: only row 2 updated.
permutation s1b s1u s2rc s2upd s1c s2c s3r
# DELETE racing FOR UPDATE: the deleted row is not returned.
permutation s1b s1d s2rc s2fu s1c s2c s3r
# The updater rolls back: the original row is locked and returned.
permutation s1b s1u s2rc s2fu s1a s2c s3r
permutation s1b s1u s2rc s2upd s1a s2c s3r
# REPEATABLE READ: an update committed after the snapshot is a serialization
# failure, whether the locker waited for it or met it already committed.
permutation s2rr s2snap s1b s1u s2fu s1c s2c s3r
permutation s2rr s2snap s1b s1u s1c s2fu s2c s3r
permutation s2rr s2snap s1b s1u s1c s2fnk s2c s3r
permutation s2rr s2snap s1b s1u s1c s2fs s2c s3r
permutation s2rr s2snap s1b s1u s1c s2sl s2c s3r
permutation s2rr s2snap s1b s1u s1c s2nw s2c s3r
permutation s2rr s2snap s1b s1u s1c s2upd s2c s3r
# REPEATABLE READ FOR KEY SHARE after a committed non-key UPDATE: no failure.
permutation s2rr s2snap s1b s1u s1c s2fks s2c s3r
# REPEATABLE READ FOR KEY SHARE after a committed key UPDATE: failure.
permutation s2rr s2snap s1b s1k s1c s2fks s2c s3r
# REPEATABLE READ, the updater rolls back while the locker waits: no failure.
permutation s2rr s2snap s1b s1u s2fu s1a s2c s3r
# REPEATABLE READ, a committed DELETE: "concurrent delete".
permutation s2rr s2snap s1b s1d s2fu s1c s2c s3r
# FK check (ri_LockPKTuple): the parent key changes while the child insert
# waits, so the insert fails.
permutation s1b s1pk s2rc s2ci s1c s2c s3c
# FK check: a non-key parent UPDATE commits while the child insert waits on
# FOR UPDATE; the key still matches, so the insert succeeds.
permutation s1b s1pfu s2rc s2ci s1pn s1c s2c s3c
# FK check: the parent key update rolls back; the insert succeeds.
permutation s1b s1pk s2rc s2ci s1a s2c s3c
# GetTupleForTrigger: the BEFORE UPDATE trigger row is locked after a
# concurrent update; EvalPlanQual drops it, so neither trigger nor UPDATE runs.
permutation s1b s1tu s2rc s2tu s1c s2c s3t
# ... and after a rollback the original row is updated.
permutation s1b s1tu s2rc s2tu s1a s2c s3t
# GetTupleForTrigger under REPEATABLE READ: serialization failure.
permutation s2rr s2snap s1b s1tu s2tu s1c s2c s3t
# A join repeats row 1: every repetition is re-checked and dropped.
permutation s1b s1u s2rc s2jfu s1c s2c s3r
# MERGE + BEFORE UPDATE trigger after a concurrent committed update.
permutation s1b s1tu s2rc s2mq s1c s2c s3t
permutation s1b s1tu s2rc s2mu s1c s2c s3t
permutation s2rr s2snap s1b s1tu s2mu s1c s2c s3t
# ON CONFLICT DO UPDATE (flags 0), READ COMMITTED: waits, then updates the
# committed version, without looping.
permutation s1b s1u s2rc s2oc s1c s2c s3r
# ON CONFLICT DO UPDATE, REPEATABLE READ: serialization failure.
permutation s2rr s2snap s1b s1u s2oc s1c s2c s3r
permutation s2rr s2snap s1b s1u s1c s2oc s2c s3r

# Two updates committed after the locker's snapshot: a key change, then a
# non-key change.  The key change conflicts with FOR KEY SHARE although the
# latest update left the key alone.  REPEATABLE READ: serialization failure,
# for the lock and for the FK check.
permutation s2rr s2snap s4pkk s4pn101 s2pks s2c s3p
permutation s2rr s2snap s4pkk s4pn101 s2ci s2c s3c
permutation s2rr s2snap s4pkk s4pn101 s2pnk s2c s3p
# READ COMMITTED: the lock moves to the latest version and re-checks it: the
# row no longer has id = 1 and drops out, a query without that qual returns
# the latest version, and the FK insert fails because the key has gone.
permutation s2rc s2dks s4pkk s4pn101 s2fc s2ci s2c s3c
permutation s2rc s2dka s4pkk s4pn101 s2fc s2c s3p
permutation s2rc s2dnk s4pkk s4pn101 s2fc s2c s3p
permutation s2rc s4pkk s4pn101 s2ci s2c s3c
# The other order: non-key change, then key change (the latest).
permutation s2rr s2snap s4pn1 s4pkk s2pks s2c s3p
permutation s2rc s2dka s4pn1 s4pkk s2fc s2c s3p
# Two non-key changes: FOR KEY SHARE conflicts with neither.
permutation s2rr s2snap s4pn1 s4pn1 s2pks s2ci s2c s3c
permutation s2rc s2dka s4pn1 s4pn1 s2fc s2c s3p
# Three updates: key change, key changed back, non-key change.  The key
# changes conflict although the key ends where it started.
permutation s2rr s2snap s4pkk s4pkb s4pn1 s2pks s2c s3p
permutation s2rr s2snap s4pkk s4pkb s4pn1 s2ci s2c s3c
permutation s2rc s2dka s4pkk s4pkb s4pn1 s2fc s2c s3p
permutation s2rc s2dks s4pkk s4pkb s4pn1 s2fc s2ci s2c s3c
# Three updates: key change, then two non-key changes.
permutation s2rr s2snap s4pkk s4pn101 s4pn101 s2pks s2c s3p
permutation s2rc s2dka s4pkk s4pn101 s4pn101 s2fc s2c s3p
# One committed, one in progress: committed key change, in-progress non-key
# change.  REPEATABLE READ fails at once, without waiting for the in-progress
# update, for FOR KEY SHARE and FOR NO KEY UPDATE alike; READ COMMITTED waits
# for it, then locks the latest version.
permutation s2rr s2snap s4pkk s5b s5pn101 s2pks s5c s2c s3p
permutation s2rr s2snap s4pkk s5b s5pn101 s2ci s5c s2c s3c
permutation s2rr s2snap s4pkk s5b s5pn101 s2pnk s5c s2c s3p
permutation s2rc s2dka s4pkk s5b s5pn101 s2fc s5c s2c s3p
permutation s2rc s2dnk s4pkk s5b s5pn101 s2fc s5c s2c s3p
# ... committed non-key change, in-progress key change: FOR KEY SHARE waits,
# then fails (commit) or locks the snapshot's version (rollback).
permutation s2rr s2snap s4pn1 s5b s5pkk s2pks s5c s2c s3p
permutation s2rr s2snap s4pn1 s5b s5pkk s2pks s5a s2c s3p
permutation s2rc s2dka s4pn1 s5b s5pkk s2fc s5c s2c s3p
permutation s2rc s2dka s4pn1 s5b s5pkk s2fc s5a s2c s3p
# Both in progress in one transaction: key change, then non-key change.  The
# FK check must wait for the key change, then fail.
permutation s5b s5pkk s5pn101 s2rc s2ci s5c s2c s3c
permutation s5b s5pkk s5pn101 s2rc s2ci s5a s2c s3c
permutation s5b s5pkk s5pn101 s2rr s2snap s2pks s5c s2c s3p
permutation s5b s5pkk s5pn101 s2rr s2snap s2pks s5a s2c s3p
# Committed non-key change, in-progress non-key change: FOR KEY SHARE
# conflicts with neither and does not wait.
permutation s2rr s2snap s4pn1 s5b s5pn1 s2pks s2ci s5c s2c s3c
permutation s2rc s2dka s4pn1 s5b s5pn1 s2fc s5c s2c s3p
# A key change the snapshot sees, then a non-key change it does not: the
# second update did not change the key, whatever the first one did.
permutation s4pkk s2rr s2snap s4pn101 s2pks101 s2ci101 s2c s3c
permutation s4pkk s2rr s2snap s5b s5pn101 s2pks101 s2ci101 s5c s2c s3c
# A key change that rolled back, then a committed non-key change: no conflict.
permutation s2rr s2snap s5b s5pkk s5a s4pn1 s2pks s2ci s2c s3c
permutation s2rc s2dka s5b s5pkk s5a s4pn1 s2fc s2c s3p
# FOR NO KEY UPDATE against the same histories: it conflicts with every
# update, key or not, committed or in progress.
permutation s2rr s2snap s4pn1 s4pn1 s2pnk s2c s3p
permutation s2rc s2dnk s4pn1 s4pn1 s2fc s2c s3p
permutation s2rr s2snap s4pn1 s5b s5pkk s2pnk s5c s2c s3p
permutation s2rr s2snap s4pn1 s5b s5pkk s2pnk s5a s2c s3p
permutation s2rr s2snap s5b s5pkk s5a s4pn1 s2pnk s2c s3p
permutation s2rr s2snap s4pkk s4pkb s4pn1 s2pnk s2c s3p

# ON CONFLICT DO UPDATE with a BEFORE UPDATE trigger, READ COMMITTED, against
# a row updated and committed after the snapshot: ON CONFLICT locks the latest
# version and the trigger's lock of the same row reports nothing, as in heap
# (no EvalPlanQual: ON CONFLICT has none set up).  Also with the row updated
# twice, with a trigger that modifies NEW, after a rollback, and under
# REPEATABLE READ.
permutation s1b s1tu s2rc s2toc s1c s2c s3t
permutation s1b s1tu s1tu2 s2rc s2toc s1c s2c s3t
permutation s1b s1tu s2rc s2toc s1a s2c s3t
permutation s2rr s2snap s1b s1tu s2toc s1c s2c s3t
permutation s1b s1nu s2rc s2noc s1c s2c s3n
permutation s1b s1nu s2rc s2noc s1a s2c s3n
# ON CONFLICT against a key update by a FOR UPDATE locker ON CONFLICT was
# waiting for: the row no longer conflicts, so the INSERT goes ahead.
permutation s1b s1lfu s2rc s2oc s1k s1c s2c s3r
permutation s1b s1tfu s2rc s2toc s1tk s1c s2c s3t
# ... and against a non-key update from that locker: the new version is
# updated (and its trigger fired once); a lock only: the row is updated.
permutation s1b s1lfu s2rc s2oc s1u s1c s2c s3r
permutation s1b s1tfu s2rc s2toc s1tu s1c s2c s3t
permutation s1b s1lfu s2rc s2oc s1c s2c s3r
permutation s2rr s2snap s1b s1lfu s2oc s1u s1c s2c s3r
# MERGE WHEN MATCHED UPDATE/DELETE after a concurrent committed update,
# without a trigger and with a trigger that modifies NEW.
permutation s1b s1u s2rc s2mlu s1c s2c s3r
permutation s1b s1u s2rc s2mlq s1c s2c s3r
permutation s1b s1u s2rc s2mld s1c s2c s3r
permutation s1b s1nu s2rc s2mnu s1c s2c s3n
permutation s1b s1nu s2rc s2mnq s1c s2c s3n
permutation s1b s1nu s2rc s2mnd s1c s2c s3n
permutation s2rr s2snap s1b s1nu s2mnu s1c s2c s3n
# Cross-partition UPDATE after a concurrent committed update, without and
# with BEFORE UPDATE/DELETE triggers, by UPDATE and by MERGE.
permutation s1b s1pu s2rc s2pm s1c s2c s3pt
permutation s1b s1pu s2rc s2pmq s1c s2c s3pt
permutation s1b s1ptu s2rc s2ptm s1c s2c s3pt
permutation s1b s1ptu s2rc s2ptmq s1c s2c s3pt
permutation s1b s1pu s2rc s2mpm s1c s2c s3pt
permutation s1b s1ptu s2rc s2mptm s1c s2c s3pt
permutation s2rr s2snap s1b s1ptu s2ptm s1c s2c s3pt
# RI ON DELETE CASCADE against a child row updated concurrently.
permutation s1b s1qcu s2rc s2qd s1c s2c s3q

# Lock first, UPDATE second: key changes conflict with all four strengths,
# but a non-key change remains compatible with KEY SHARE (including CAS).
permutation s2rc s2fks s1b s1k s2c s1c s3r
permutation s2rc s2fks s1b s1u s1c s2c s3r
permutation s2rc s2fs s1b s1k s2c s1c s3r
permutation s2rc s2fnk s1b s1k s2c s1c s3r
permutation s2rc s2fu s1b s1k s2c s1c s3r
# FK insertion holds KEY SHARE until commit.  The parent UPDATE must wait
# and then check the newly committed child; an aborted child permits it.
permutation s2rc s2ci s1b s1pk s2c s1c s3p s3c s3orphans
permutation s2rc s2ci s1b s1pk s2a s1c s3p s3c s3orphans
permutation s2rc s2uci s1b s1uk s2c s1c s3u
permutation s2rc s2rci s1b s1rk s2c s1c s3restrict
# Subtransaction rollback restores the parent and releases its key lock.
permutation s2rc s2fks s1b s1sp s1k s2c s1rb s1c s3r
permutation s1b s1sp s1pk s2rc s2ci s1rb s2c s1c s3p s3c s3orphans
# A single component change, and a standalone unique index used by an FK.
permutation s2rc s2mci s1b s1mk s2c s1c s3multi
permutation s2rc s2vci s1b s1vk s2c s1c s3unique
permutation s2rc s2vci s1b s1vn s1c s2c s3unique

# A compatible non-key update clears the on-page lock flag, not the FK
# lock in sLog.  The same-size unique-key update can now reach CAS.
permutation s2rc s2vci s4vn s1b s1vk s2c s1c s3unique
