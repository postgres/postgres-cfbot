# FLUX: REPACK (CONCURRENTLY) with concurrent data changes.
#
# REPACK (CONCURRENTLY) copies the rows its initial snapshot sees into a new
# table, then replays onto it, by replica identity key, the changes logical
# decoding finds committed after that snapshot.  FLUX updates rows in place
# (also when a key changes), writes the old version to UNDO, and reverts an
# aborted transaction in place from UNDO; the copy must still read exactly
# what the snapshot sees, and the replay must neither miss a committed change
# nor apply an aborted one.
#
# Each scenario runs twice, on a FLUX table and on its heap twin (pick_fx or
# pick_hp renames one of them, and its indexes, to t); heap is the control
# and both must give the same answers.  The changes are made while REPACK
# waits at repack-concurrently-before-lock (after the copy and the index
# builds, before the catch-up), unless a scenario says otherwise.  "truth"
# saves what a reader sees just before REPACK resumes; chk() then reports
# whether the table was rewritten, a digest of its contents, how many rows
# differ from truth, whether an index scan of each index returns what a
# seqscan does, and amcheck (heapallindexed, checkunique) of every index.
#
# Row 10 sits on a full first page and v is stored PLAIN, so growing v moves
# the row (FLUX: out-of-place UPDATE, logged as XLOG_FLUX_SUPERSEDE); w is
# stored EXTERNAL, so rows 1-5 have their w in the TOAST table.

setup
{
  CREATE EXTENSION amcheck;
  CREATE EXTENSION injection_points;
  CREATE TABLE fx (id int PRIMARY KEY, k int, v text, w text) USING flux
	WITH (fillfactor = 100, autovacuum_enabled = off);
  CREATE TABLE hp (id int PRIMARY KEY, k int, v text, w text) USING heap
	WITH (fillfactor = 100, autovacuum_enabled = off);
  ALTER TABLE fx ALTER COLUMN v SET STORAGE PLAIN,
	ALTER COLUMN w SET STORAGE EXTERNAL;
  ALTER TABLE hp ALTER COLUMN v SET STORAGE PLAIN,
	ALTER COLUMN w SET STORAGE EXTERNAL;
  CREATE INDEX fx_k ON fx (k);
  CREATE INDEX hp_k ON hp (k);
  INSERT INTO fx SELECT g, g, repeat('a', 100),
	CASE WHEN g <= 5 THEN repeat(md5(g::text), 100) END
	FROM generate_series(1, 120) g;
  INSERT INTO hp SELECT * FROM fx;

  CREATE TABLE truth (id int, k int, v text, w text) USING heap;
  -- other tables, changed during REPACK: their changes are not t's
  CREATE TABLE ofx (id int PRIMARY KEY, k int, v text, w text) USING flux;
  CREATE TABLE ohp (id int PRIMARY KEY, k int, v text, w text) USING heap;
  INSERT INTO ofx SELECT * FROM fx WHERE id <= 30;
  INSERT INTO ohp SELECT * FROM fx WHERE id <= 30;
  CREATE TABLE rf (node oid) USING heap;

  -- wait until REPACK holds at the injection point (copy and index builds
  -- done, catch-up not started)
  CREATE FUNCTION wait_hold() RETURNS void LANGUAGE plpgsql AS $$
  BEGIN
    FOR i IN 1 .. 30000 LOOP
      PERFORM pg_stat_clear_snapshot();
      IF EXISTS (SELECT 1 FROM pg_stat_activity
                 WHERE wait_event_type = 'InjectionPoint'
                   AND wait_event = 'repack-concurrently-before-lock') THEN
        RETURN;
      END IF;
      PERFORM pg_sleep(0.01);
    END LOOP;
    RAISE EXCEPTION 'REPACK did not reach the injection point';
  END $$;

  -- wait until some process waits for the xid of isolation session sess
  CREATE FUNCTION wait_for(sess text) RETURNS void LANGUAGE plpgsql AS $$
  BEGIN
    FOR i IN 1 .. 30000 LOOP
      IF EXISTS (SELECT 1 FROM pg_locks w JOIN pg_locks h
                 ON h.locktype = 'transactionid'
                    AND h.transactionid = w.transactionid AND h.granted
                 JOIN pg_stat_activity a ON a.pid = h.pid
                 WHERE w.locktype = 'transactionid' AND NOT w.granted
                   AND a.application_name LIKE '%/' || sess) THEN
        RETURN;
      END IF;
      PERFORM pg_sleep(0.01);
    END LOOP;
    RAISE EXCEPTION 'nobody waited for session %', sess;
  END $$;

  CREATE FUNCTION chk() RETURNS TABLE (rewritten bool, nrows bigint,
	sum_id bigint, sum_k bigint, len_v bigint, len_w bigint,
	digest text, vs_truth bigint, pk_scan text, k_scan text, amcheck text)
  LANGUAGE plpgsql AS $$
  DECLARE
    seq_pk text;
    seq_k text;
    idx_pk text;
    idx_k text;
  BEGIN
    rewritten := (SELECT c.relfilenode <> (SELECT node FROM rf)
                  FROM pg_class c WHERE c.relname = 't');
    SELECT count(*), sum(id), sum(k), sum(length(v)), sum(length(w)),
           md5(string_agg(format('%s/%s/%s/%s', id, k, md5(v), md5(w)),
                          ',' ORDER BY id))
      INTO nrows, sum_id, sum_k, len_v, len_w, digest FROM t;
    vs_truth := (SELECT count(*) FROM
                 ((SELECT * FROM t EXCEPT ALL SELECT * FROM truth)
                  UNION ALL
                  (SELECT * FROM truth EXCEPT ALL SELECT * FROM t)) d);
    PERFORM set_config('enable_indexscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    SELECT string_agg(format('%s:%s', id, k), ',' ORDER BY id) INTO seq_pk
      FROM t WHERE id > -1000000;
    SELECT string_agg(format('%s:%s', id, k), ',' ORDER BY id) INTO seq_k
      FROM t WHERE k > -1000000;
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_indexonlyscan', 'off', true);
    SELECT string_agg(format('%s:%s', id, k), ',' ORDER BY id) INTO idx_pk
      FROM t WHERE id > -1000000;
    SELECT string_agg(format('%s:%s', id, k), ',' ORDER BY id) INTO idx_k
      FROM t WHERE k > -1000000;
    pk_scan := CASE WHEN idx_pk IS NOT DISTINCT FROM seq_pk THEN 'same'
                    ELSE 'differs' END;
    k_scan := CASE WHEN idx_k IS NOT DISTINCT FROM seq_k THEN 'same'
                   ELSE 'differs' END;
    amcheck := 'ok';
    BEGIN
      PERFORM bt_index_check(i.indexrelid, true, true)
        FROM pg_index i WHERE i.indrelid = 't'::regclass;
    EXCEPTION WHEN OTHERS THEN
      amcheck := SQLERRM;
    END;
    RETURN NEXT;
  END $$;
}

teardown
{
  DROP TABLE IF EXISTS fx, hp, t, truth, rf, ofx, ohp;
  DROP FUNCTION wait_hold, wait_for, chk;
  DROP EXTENSION amcheck;
  DROP EXTENSION injection_points;
}

# The REPACK session.  The injection point is attached for every process
# (injection_points_set_local() registers an exit callback each time a
# permutation starts, and this spec has more permutations than there are
# slots); only REPACK reaches it.
session s
setup
{
  RESET enable_sort;
  RESET enable_indexscan;
  SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step pick_fx	{
  ALTER TABLE fx RENAME TO t;
  ALTER INDEX fx_pkey RENAME TO t_pkey;
  ALTER INDEX fx_k RENAME TO t_k;
  INSERT INTO rf SELECT relfilenode FROM pg_class WHERE relname = 't';
}
step pick_hp	{
  ALTER TABLE hp RENAME TO t;
  ALTER INDEX hp_pkey RENAME TO t_pkey;
  ALTER INDEX hp_k RENAME TO t_k;
  INSERT INTO rf SELECT relfilenode FROM pg_class WHERE relname = 't';
}
step by_idx	{ SET enable_sort = off; }
step by_sort	{ SET enable_indexscan = off; }
step rp		{ REPACK (CONCURRENTLY) t; }
step rpi	{ REPACK (CONCURRENTLY) t USING INDEX t_pkey; }
step chk	{ SELECT * FROM chk(); }
teardown
{
  SELECT injection_points_detach('repack-concurrently-before-lock');
}

# Writers.
session w
step wb		{ BEGIN; }
step wc		{ COMMIT; }
step wr		{ ROLLBACK; }
step wxid	{ BEGIN; SELECT pg_current_xact_id() IS NOT NULL AS has_xid; }
step w_ins	{ INSERT INTO t VALUES (1001, 1001, 'new', NULL); }
step w_nonkey	{ UPDATE t SET v = repeat('n', 100) WHERE id = 20; }
step w_k	{ UPDATE t SET k = 2100 WHERE id = 21; }
step w_id	{ UPDATE t SET id = 1022 WHERE id = 22; }
step w_grow	{
  UPDATE t SET v = repeat('g', 2000) WHERE id = 10;
  SELECT (ctid::text::point)[0] > 0 AS moved FROM t WHERE id = 10;
}
step w_del	{ DELETE FROM t WHERE id = 23; }
step w_aba	{
  UPDATE t SET id = 1030 WHERE id = 30;
  UPDATE t SET k = 3000 WHERE id = 1030;
  UPDATE t SET id = 30 WHERE id = 1030;
}
step w_life	{
  INSERT INTO t VALUES (1040, 1040, 'life', NULL);
  UPDATE t SET k = 1041, v = 'life2' WHERE id = 1040;
  UPDATE t SET id = 1042 WHERE id = 1040;
  DELETE FROM t WHERE id = 1042;
}
step w_a2b	{ UPDATE t SET id = 1031 WHERE id = 31; }
step w_b2a	{ UPDATE t SET id = 31, k = 3100 WHERE id = 1031; }
step w_ins2	{ INSERT INTO t VALUES (1050, 1050, 'x', NULL); }
step w_upd2	{ UPDATE t SET k = 1051, id = 1052 WHERE id = 1050; }
step w_del2	{ DELETE FROM t WHERE id = 1052; }
step w_sp	{
  UPDATE t SET k = 4000 WHERE id = 40;
  SAVEPOINT a;
  UPDATE t SET k = 4001, id = 1040 WHERE id = 40;
  DELETE FROM t WHERE id = 41;
  INSERT INTO t VALUES (1043, 1043, 'sp', NULL);
  UPDATE t SET v = repeat('s', 2000) WHERE id = 11;
  ROLLBACK TO SAVEPOINT a;
  UPDATE t SET v = 'after' WHERE id = 41;
  SAVEPOINT b;
  INSERT INTO t VALUES (1044, 1044, 'kept', NULL);
  RELEASE SAVEPOINT b;
}
step w_upsert	{
  INSERT INTO t VALUES (50, 0, 'u', NULL)
	ON CONFLICT (id) DO UPDATE SET k = excluded.k + 5000, v = 'upd';
  INSERT INTO t VALUES (1060, 1060, 'u', NULL)
	ON CONFLICT (id) DO UPDATE SET k = 0;
  INSERT INTO t VALUES (51, 0, 'n', NULL) ON CONFLICT DO NOTHING;
  INSERT INTO t VALUES (1061, 1061, 'n', NULL) ON CONFLICT DO NOTHING;
  INSERT INTO t VALUES (52, 0, 'u', NULL)
	ON CONFLICT (id) DO UPDATE SET id = 1062;
}
step w_toast	{
  INSERT INTO t VALUES (1070, 1070, 'toast', repeat(md5('new'), 100));
  UPDATE t SET w = repeat(md5('upd'), 100) WHERE id = 1;
  UPDATE t SET k = 2002 WHERE id = 2;
  UPDATE t SET id = 1003 WHERE id = 3;
  DELETE FROM t WHERE id = 4;
  UPDATE t SET w = NULL WHERE id = 5;
  UPDATE t SET w = repeat(md5('grown'), 100) WHERE id = 60;
}
step w_prep	{ PREPARE TRANSACTION 'rp1'; }
step w_same	{ UPDATE t SET v = repeat('c', 100) WHERE id = 70; }
step w_after	{ UPDATE t SET v = 'later', k = 2200 WHERE id = 22; }
step w_other	{
  INSERT INTO ofx VALUES (1001, 1, 'other', NULL);
  UPDATE ofx SET k = 2100, v = repeat('o', 2000) WHERE id = 21;
  UPDATE ofx SET id = 1022 WHERE id = 22;
  DELETE FROM ofx WHERE id = 23;
  INSERT INTO ohp VALUES (1001, 1, 'other', NULL);
  UPDATE ohp SET k = 2100, v = repeat('o', 2000) WHERE id = 21;
  UPDATE ohp SET id = 1022 WHERE id = 22;
  DELETE FROM ohp WHERE id = 23;
}

# Transactions that keep an xid open, so that the decoding's snapshot builder
# has to wait for them (see repack_commit_race.spec).
session x2
step x2b	{ BEGIN; SELECT pg_current_xact_id() IS NOT NULL AS has_xid; }
step x2r	{ ROLLBACK; }
session x3
step x3b	{ BEGIN; SELECT pg_current_xact_id() IS NOT NULL AS has_xid; }
step x3r	{ ROLLBACK; }

# Holds REPACK between its snapshot and its copy: REPACK names its new table
# pg_temp_<oid of t>, so while g has created a table of that name and not
# committed, REPACK waits for g in make_new_heap.  g gets its xid only after
# the snapshot builder stopped waiting for new transactions (x3), so REPACK's
# snapshot does not wait for g.
session g
step gb	{
  BEGIN;
  DO $$ BEGIN
    EXECUTE format('CREATE TABLE %I (x int)', 'pg_temp_' || 't'::regclass::oid);
  END $$;
}
step gr	{ ROLLBACK; }

# A transaction prepared before REPACK starts.
session p
step pp	{
  BEGIN;
  UPDATE t SET k = 8000, v = repeat('p', 100) WHERE id = 80;
  UPDATE t SET id = 1081 WHERE id = 81;
  PREPARE TRANSACTION 'rp0';
}
step pc0	{ COMMIT PREPARED 'rp0'; }
step pc1	{ COMMIT PREPARED 'rp1'; }

# An old snapshot.
session r
step rrb	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1 AS snap; }
step rrs	{
  SELECT count(*) AS old_snapshot_rows, sum(k) AS old_snapshot_sum_k,
	(SELECT k FROM t WHERE id = 21) AS k21 FROM t;
}
step rrc	{ COMMIT; }

# Observer: waits for the hold, saves the expected contents, resumes REPACK.
session o
step hold	{ SELECT wait_hold(); }
step wx3	{ SELECT wait_for('x3'); }
step wg	{ SELECT wait_for('g'); }
step truth	{ INSERT INTO truth SELECT * FROM t; }
step wake	{
  SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

# F1: INSERT, non-key UPDATE, secondary-key UPDATE, identity-key UPDATE, an
# UPDATE that moves the row, DELETE; each its own transaction.
permutation pick_fx rp hold w_ins w_nonkey w_k w_id w_grow w_del truth wake chk
permutation pick_hp rp hold w_ins w_nonkey w_k w_id w_grow w_del truth wake chk
# Other tables, FLUX and heap, changed by the same keys during REPACK: none
# of their changes may reach t.
permutation pick_fx rp hold w_other w_k truth wake chk
permutation pick_hp rp hold w_other w_k truth wake chk
permutation pick_fx rp hold wb w_other w_ins wc truth wake chk
permutation pick_hp rp hold wb w_other w_ins wc truth wake chk
# The same, copying in index order and through a sort.
permutation pick_fx by_idx rpi hold w_ins w_k w_id w_grow w_del truth wake chk
permutation pick_hp by_idx rpi hold w_ins w_k w_id w_grow w_del truth wake chk
permutation pick_fx by_sort rpi hold w_ins w_k w_id w_grow w_del truth wake chk
permutation pick_hp by_sort rpi hold w_ins w_k w_id w_grow w_del truth wake chk
# The same changes in one transaction.
permutation pick_fx rp hold wb w_ins w_nonkey w_k w_id w_grow w_del wc truth wake chk
permutation pick_hp rp hold wb w_ins w_nonkey w_k w_id w_grow w_del wc truth wake chk

# F2: one row changed many times: A->B->A of the identity key in one
# transaction and across transactions; INSERT, UPDATEs and DELETE of a new
# row in one transaction and across transactions.
permutation pick_fx rp hold w_aba w_life w_a2b w_b2a w_ins2 w_upd2 w_del2 truth wake chk
permutation pick_hp rp hold w_aba w_life w_a2b w_b2a w_ins2 w_upd2 w_del2 truth wake chk
permutation pick_fx rp hold wb w_aba w_life w_a2b w_b2a w_ins2 w_upd2 w_del2 wc truth wake chk
permutation pick_hp rp hold wb w_aba w_life w_a2b w_b2a w_ins2 w_upd2 w_del2 wc truth wake chk

# F3: a transaction that changes everything and rolls back during REPACK
# (its revert runs at once), then committed changes of the same rows.
permutation pick_fx rp hold wb w_ins w_nonkey w_k w_id w_grow w_del w_aba wr w_after truth wake chk
permutation pick_hp rp hold wb w_ins w_nonkey w_k w_id w_grow w_del w_aba wr w_after truth wake chk

# F4: ROLLBACK TO SAVEPOINT inside a committed transaction.
permutation pick_fx rp hold wb w_sp wc truth wake chk
permutation pick_hp rp hold wb w_sp wc truth wake chk

# F5: INSERT ... ON CONFLICT DO UPDATE / DO NOTHING.
permutation pick_fx rp hold w_upsert truth wake chk
permutation pick_hp rp hold w_upsert truth wake chk

# F6: TOAST: wide values inserted, replaced, kept across an UPDATE, deleted.
permutation pick_fx rp hold w_toast truth wake chk
permutation pick_hp rp hold w_toast truth wake chk

# F7: a transaction prepared and committed during REPACK, and one prepared
# before REPACK starts, whose COMMIT PREPARED REPACK's snapshot waits for.
permutation pick_fx rp hold wb w_k w_id w_grow w_prep pc1 truth wake chk
permutation pick_hp rp hold wb w_k w_id w_grow w_prep pc1 truth wake chk
permutation pick_fx pp rp(*) pc0 hold w_ins truth wake chk
permutation pick_hp pp rp(*) pc0 hold w_ins truth wake chk

# F8: snapshots taken before REPACK starts, and during it, used after it
# committed.  REPACK (CONCURRENTLY) is not MVCC-safe: like heap, FLUX must
# show such a snapshot an empty table.
permutation pick_fx rrb rp hold w_k truth wake chk rrs rrc
permutation pick_hp rrb rp hold w_k truth wake chk rrs rrc
permutation pick_fx rp hold w_k rrb truth wake chk rrs rrc
permutation pick_hp rp hold w_k rrb truth wake chk rrs rrc

# Changes in progress while REPACK copies: the snapshot builder waits for x2,
# then x3; w gets its xid once the builder waits for x3, so w is still running
# when the snapshot is taken, and its changes are decoded.  The copy must read the
# versions before them (FLUX: from UNDO), and commit or rollback during the
# hold decides whether they are replayed.
permutation pick_fx x2b rp x3b x2r wx3 wb w_nonkey w_k w_id w_grow w_del w_ins x3r hold wc truth wake chk
permutation pick_hp x2b rp x3b x2r wx3 wb w_nonkey w_k w_id w_grow w_del w_ins x3r hold wc truth wake chk
permutation pick_fx x2b rp x3b x2r wx3 wb w_nonkey w_k w_id w_grow w_del w_ins x3r hold wr w_after truth wake chk
permutation pick_hp x2b rp x3b x2r wx3 wb w_nonkey w_k w_id w_grow w_del w_ins x3r hold wr w_after truth wake chk

# Changes committed after REPACK's snapshot, before its copy: the copy must
# read the versions the snapshot sees (FLUX: from UNDO) and the replay apply
# the changes once.  Also a transaction that rolls back in that window, and
# one running at the snapshot that commits in it.
permutation pick_fx x2b rp x3b x2r wx3 gb x3r wg w_ins w_nonkey w_k w_id w_grow w_del w_aba gr hold truth wake chk
permutation pick_hp x2b rp x3b x2r wx3 gb x3r wg w_ins w_nonkey w_k w_id w_grow w_del w_aba gr hold truth wake chk
permutation pick_fx x2b rp x3b x2r wx3 wb w_ins w_k w_id w_grow gb x3r wg wc w_toast w_upsert gr hold truth wake chk
permutation pick_hp x2b rp x3b x2r wx3 wb w_ins w_k w_id w_grow gb x3r wg wc w_toast w_upsert gr hold truth wake chk
permutation pick_fx x2b rp x3b x2r wx3 gb x3r wg wb w_ins w_nonkey w_k w_id w_grow w_del wr w_after gr hold truth wake chk
permutation pick_hp x2b rp x3b x2r wx3 gb x3r wg wb w_ins w_nonkey w_k w_id w_grow w_del wr w_after gr hold truth wake chk

# The same window, copying through the identity index and through a sort:
# key UPDATEs after the snapshot leave delete-marked entries under the old
# keys (FLUX), and each row must still be copied once.
permutation pick_fx by_idx x2b rpi x3b x2r wx3 gb x3r wg w_id w_aba w_a2b w_k w_del w_ins gr hold truth wake chk
permutation pick_hp by_idx x2b rpi x3b x2r wx3 gb x3r wg w_id w_aba w_a2b w_k w_del w_ins gr hold truth wake chk
permutation pick_fx by_sort x2b rpi x3b x2r wx3 gb x3r wg w_id w_aba w_a2b w_k w_del w_ins gr hold truth wake chk
permutation pick_hp by_sort x2b rpi x3b x2r wx3 gb x3r wg w_id w_aba w_a2b w_k w_del w_ins gr hold truth wake chk

# F11 (wal_level = replica, so REPACK raises the effective level): w has an
# xid before REPACK enables logical decoding, so w keeps logging without
# logical images (FLUX may take its same-size fast path) until it ends.
# REPACK's snapshot waits for w, so w's changes must be in the copy.
permutation pick_fx wxid rp w_same w_k wc hold w_ins truth wake chk
permutation pick_hp wxid rp w_same w_k wc hold w_ins truth wake chk
