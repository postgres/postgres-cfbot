# FLUX: CREATE INDEX CONCURRENTLY and REINDEX CONCURRENTLY under concurrent
# key UPDATEs.
#
# FLUX updates a row in place, at the same TID, even when an indexed column
# changes: it inserts the new (key, TID) entry and delete-marks the old one
# in every ready index.  A concurrent build cannot rely on that for the index
# it is building.  Its phase 2 indexes the rows a snapshot sees, and its
# validation (phase 3) adds only the TIDs the index does not have yet.  A key
# UPDATE that sees the new index not yet ready skips it, so if the build's
# snapshot missed that UPDATE, the index keeps the old key for a TID the
# validation then skips.  Heap avoids this because such an UPDATE is never HOT:
# the new version gets a new TID, which the validation finds missing.
#
# Each scenario runs twice, on a FLUX table and on its heap twin (pick_fx or
# pick_hp renames one of them to t); heap is the control and both must give
# the same answers.  chk() compares an index scan with a seqscan for a qual
# and runs amcheck (heapallindexed, checkunique) on every valid index of t,
# reporting its error if it finds one.
#
# The windows between the phases are held open by lock holders: blocker b1
# holds RowExclusiveLock before the build starts, so the build waits for it
# (phase 2 wait) after its catalog entry is visible but before it takes its
# snapshot; b2 takes the lock while the build waits, so the build later waits
# for it with the index ready (phase 3 wait) before validating.  For
# REINDEX CONCURRENTLY the swap is held at an injection point.

setup
{
  CREATE EXTENSION amcheck;
  CREATE EXTENSION injection_points;
  CREATE TABLE fx (id int PRIMARY KEY, k int, v int) USING flux
	WITH (autovacuum_enabled = off);
  CREATE TABLE hp (id int PRIMARY KEY, k int, v int) USING heap
	WITH (autovacuum_enabled = off);
  INSERT INTO fx SELECT g, g, g FROM generate_series(1, 20) g;
  INSERT INTO hp SELECT g, g, g FROM generate_series(1, 20) g;

  -- whether row id is still where tid_at() last found it: an in-place
  -- UPDATE keeps the TID, a moved row has a new one
  CREATE TABLE tid_seen (id int PRIMARY KEY, tid tid);
  CREATE FUNCTION tid_at(rid int) RETURNS void LANGUAGE plpgsql AS $$
  BEGIN
    INSERT INTO tid_seen SELECT id, ctid FROM t WHERE id = rid
      ON CONFLICT (id) DO UPDATE SET tid = excluded.tid;
  END $$;
  CREATE FUNCTION tid_kept(rid int) RETURNS bool LANGUAGE plpgsql AS $$
  BEGIN
    RETURN (SELECT s.tid = t.ctid FROM tid_seen s JOIN t USING (id)
            WHERE id = rid);
  END $$;

  -- ids that match qual through an index scan and through a seqscan, then
  -- amcheck every valid index of t
  CREATE FUNCTION chk(qual text) RETURNS TABLE (via text, ids text)
  LANGUAGE plpgsql AS $$
  DECLARE
    q text := format('SELECT string_agg(id::text, '','' ORDER BY id) FROM t WHERE %s', qual);
    r text;
  BEGIN
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    EXECUTE q INTO r;
    via := 'idx'; ids := r; RETURN NEXT;
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    EXECUTE q INTO r;
    via := 'seq'; ids := r; RETURN NEXT;
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    via := 'amcheck'; ids := 'ok';
    BEGIN
      PERFORM bt_index_check(i.indexrelid, true, true)
        FROM pg_index i WHERE i.indrelid = 't'::regclass AND i.indisvalid;
    EXCEPTION WHEN OTHERS THEN
      ids := SQLERRM;
    END;
    RETURN NEXT;
  END $$;
}

teardown
{
  DROP TABLE IF EXISTS fx, hp, t, tid_seen;
  DROP FUNCTION chk, tid_at, tid_kept;
  DROP EXTENSION amcheck;
  DROP EXTENSION injection_points;
}

session b1
step b1l	{ BEGIN; LOCK TABLE t IN ROW EXCLUSIVE MODE; }
step b1c	{ COMMIT; }

session b2
step b2l	{ BEGIN; LOCK TABLE t IN ROW EXCLUSIVE MODE; }
step b2c	{ COMMIT; }

session c
step c_attach	{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
}
step pick_fx	{ ALTER TABLE fx RENAME TO t; }
step pick_hp	{ ALTER TABLE hp RENAME TO t; }
step c_idx	{ CREATE INDEX t_k ON t (k); }
step c_uidx	{ CREATE UNIQUE INDEX t_k ON t (k); }
step c_cic	{ CREATE INDEX CONCURRENTLY t_k ON t (k); }
step c_ucic	{ CREATE UNIQUE INDEX CONCURRENTLY t_k ON t (k); }
step c_ecic	{ CREATE INDEX CONCURRENTLY t_k ON t ((k * 2)); }
step c_icic	{ CREATE INDEX CONCURRENTLY t_k ON t (k) INCLUDE (v); }
step c_pcic	{ CREATE INDEX CONCURRENTLY t_k ON t (k) WHERE k < 1000; }
step c_ric	{ REINDEX INDEX CONCURRENTLY t_k; }
step c_dic	{ DROP INDEX CONCURRENTLY t_k; }
step c_dup	{ UPDATE t SET k = 2 WHERE id = 1; }
step c_undup	{ UPDATE t SET k = 1 WHERE id = 1; }
step c_tid	{ SELECT tid_at(1); }
# Cannot start before the session's previous step (c_ric) has finished.
step c_done	{ }
step c_state	{
  SELECT c.relname, i.indisready, i.indisvalid FROM pg_index i
  JOIN pg_class c ON c.oid = i.indexrelid
  WHERE c.relname = 't_k';
}

session w1
step w1b	{ BEGIN; }
step w1u	{ UPDATE t SET k = 100 WHERE id = 1; }
step w1d	{ UPDATE t SET k = 2 WHERE id = 1; }
step w1v	{ UPDATE t SET v = 100 WHERE id = 1; }
step w1x	{ DELETE FROM t WHERE id = 1; }
step w1i	{ INSERT INTO t VALUES (23, 100); }
step w1n	{ UPDATE t SET v = 1000 WHERE id = 1; SELECT tid_kept(1) AS in_place; }
step w1u2	{ UPDATE t SET k = 200 WHERE id = 1; SELECT tid_kept(1) AS in_place; }
step w1c	{ COMMIT; }
step w1r	{ ROLLBACK; }

session w2
step w2b	{ BEGIN; }
step w2u	{ UPDATE t SET k = 300 WHERE id = 3; }
step w2a	{ UPDATE t SET k = 1 WHERE id = 1; }
step w2v	{ UPDATE t SET k = 500 WHERE id = 23; }
step w2r	{ ROLLBACK; }
step w2c	{ COMMIT; }
step w2i1	{ INSERT INTO t VALUES (21, 1); }
step w2i100	{ INSERT INTO t VALUES (22, 100); }

session r
step r_wake	{
  SELECT injection_points_detach('reindex-relation-concurrently-before-swap');
  SELECT injection_points_wakeup('reindex-relation-concurrently-before-swap');
}
step rrb	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snap; }
step rrc	{ COMMIT; }
step chk200	{ SELECT * FROM chk('k = 200'); }
step chk1	{ SELECT * FROM chk('k = 1'); }
step chk2	{ SELECT * FROM chk('k = 2'); }
step chk100	{ SELECT * FROM chk('k = 100'); }
step chk300	{ SELECT * FROM chk('k = 300'); }
step chk3	{ SELECT * FROM chk('k = 3'); }
step chke	{ SELECT * FROM chk('k * 2 = 200'); }
step chk500	{ SELECT * FROM chk('k = 500'); }
step chkp	{ SELECT * FROM chk('k = 100 AND k < 1000'); }

# Key UPDATE in phase 2, after the index is visible, invisible to the build
# snapshot, committed before validation.
permutation pick_fx b1l c_cic w1b w1u b1c w1c chk100 chk1
permutation pick_hp b1l c_cic w1b w1u b1c w1c chk100 chk1

# A -> B in phase 2, B -> A in phase 3 (index ready, not yet validated).
permutation pick_fx b1l c_cic w1b w1u b2l b1c w1c w2a b2c chk1 chk100
permutation pick_hp b1l c_cic w1b w1u b2l b1c w1c w2a b2c chk1 chk100

# DELETE and INSERT in phase 2.
permutation pick_fx b1l c_cic w1b w1x w1i b1c w1c chk100 chk1
permutation pick_hp b1l c_cic w1b w1x w1i b1c w1c chk100 chk1

# Key UPDATE in phase 2 that rolls back after the build.
permutation pick_fx b1l c_cic w1b w1u b1c w1r chk100 chk1
permutation pick_hp b1l c_cic w1b w1u b1c w1r chk100 chk1

# Key UPDATE in phase 3, still running through validation, rolled back
# after the index is valid: the rollback must reverse it in the new index.
permutation pick_fx b1l c_cic b2l b1c w2b w2u b2c w2r chk300 chk3
permutation pick_hp b1l c_cic b2l b1c w2b w2u b2c w2r chk300 chk3

# INSERT in phase 2, then in phase 3 (index ready) a key UPDATE of that row
# that runs through validation and rolls back after it.  The validation must
# index the version its snapshot sees, and the rollback must leave it there.
permutation pick_fx b1l c_cic w1b w1i b2l b1c w1c w2b w2v b2c w2r chk100 chk500
permutation pick_hp b1l c_cic w1b w1i b2l b1c w1c w2b w2v b2c w2r chk100 chk500

# The same UPDATE committed after a REPEATABLE READ snapshot that predates it
# was taken: the snapshot still needs the row under its old key.
permutation pick_fx b1l c_cic w1b w1i b2l b1c w1c w2b w2v b2c rrb w2c chk100 rrc chk500
permutation pick_hp b1l c_cic w1b w1i b2l b1c w1c w2b w2v b2c rrb w2c chk100 rrc chk500

# UNIQUE: an UPDATE in phase 2 makes a duplicate; the build must fail.
permutation pick_fx b1l c_ucic w1b w1d b1c w1c c_state chk2
permutation pick_hp b1l c_ucic w1b w1d b1c w1c c_state chk2

# Expression and partial indexes.
permutation pick_fx b1l c_ecic w1b w1u b1c w1c chke
permutation pick_hp b1l c_ecic w1b w1u b1c w1c chke
permutation pick_fx b1l c_pcic w1b w1u b1c w1c chkp
permutation pick_hp b1l c_pcic w1b w1u b1c w1c chkp

# An UPDATE of an INCLUDE column only.
permutation pick_fx b1l c_icic w1b w1v b1c w1c chk1
permutation pick_hp b1l c_icic w1b w1v b1c w1c chk1

# REINDEX CONCURRENTLY of a unique index with a key UPDATE in phase 2: the
# rebuilt index must still reject a duplicate of the new key and accept the
# old one.
permutation pick_fx c_uidx b1l c_ric w1b w1u b1c w1c chk100 w2i100 w2i1 chk1
permutation pick_hp c_uidx b1l c_ric w1b w1u b1c w1c chk100 w2i100 w2i1 chk1

# DROP INDEX CONCURRENTLY: a key UPDATE made while the index is still
# valid commits after the drop has cleared indisvalid; another one is made
# with the index invalid, before it is dead.
permutation pick_fx c_idx w1b w1u c_dic w1c chk100 chk1
permutation pick_hp c_idx w1b w1u c_dic w1c chk100 chk1
permutation pick_fx c_idx b1l c_dic w1b w1u b1c w1c chk100 chk1
permutation pick_hp c_idx b1l c_dic w1b w1u b1c w1c chk100 chk1

# A UNIQUE build that fails leaves an invalid index.  On FLUX a key UPDATE
# then moves the row and an UPDATE of a column no index uses stays in place
# (in_place; heap gives every updated row a new TID, so its twin says f
# both times).  REINDEX INDEX CONCURRENTLY of the invalid index, with a key
# UPDATE in its phase 2, makes it valid and complete.
permutation pick_fx c_dup c_ucic c_undup c_state c_tid w1b w1n w1u2 w1c chk200 c_tid b1l c_ric w1b w1u b1c w1c c_state chk100 chk200
permutation pick_hp c_dup c_ucic c_undup c_state c_tid w1b w1n w1u2 w1c chk200 c_tid b1l c_ric w1b w1u b1c w1c c_state chk100 chk200

# REINDEX CONCURRENTLY held before the swap; a key UPDATE made with both the
# old and the new index ready rolls back after the swap wait is released.
# This does not force the rollback to occur after the swap itself.
# After r_wake, c_ric may still be in the injection point's wait loop and
# so look blocked when w2r has finished; c_done keeps chk300 from starting
# before c_ric is done.
permutation pick_fx c_idx c_attach c_ric w2b w2u r_wake w2r c_done chk300 chk3
permutation pick_hp c_idx c_attach c_ric w2b w2u r_wake w2r c_done chk300 chk3
