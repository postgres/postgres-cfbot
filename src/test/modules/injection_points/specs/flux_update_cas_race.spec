# An UPDATE decides whether it changes an indexed column, and which index
# entries change, from the image on the page, read without waiting.  The row
# can change between that decision and the moment the UPDATE has it locked:
# a running writer can roll back, or a new one can write.  Two checks keep
# the decision tied to the version it was made from (flux_same_version):
#
# - the same-size in-place path (CAS), which overwrites the row without
#   index work when no indexed column changes, must find that version on the
#   page.  s1 changes k from 3 to 100; s2 sets k to 100 too, so it finds no
#   key change; s1 rolls back before s2 locks the page.  Overwriting the
#   restored row (k = 3) without index work would leave it with k = 100 and
#   no entry for it.
#
# - the copy of the old row that a key change forms its old index entries
#   from must be that version too.  s2 sets k to 200 and decides from the
#   committed row (k = 3); s1 then changes k to 100, so s2's copy is s1's
#   image; s1 rolls back.  Delete-marking the entry of k = 100 instead of
#   k = 3 would leave (3, TID) live, and changing k back to 3 would add a
#   second (3, TID) entry.
#
# s2 stops at flux-update-after-decision; wake detaches the point (a second
# decision does not stop) and lets s2 go on; s2done waits for s2.  The row
# is then read through the index under every key it may have and by a
# seqscan, and amcheck checks the index.  Each permutation runs on FLUX
# (pick_fx) and heap (pick_hp), which must return the same rows; heap's s2
# never stops at the injection point.

setup
{
  CREATE EXTENSION injection_points;
  CREATE EXTENSION amcheck;
  CREATE TABLE cr_fx (id int, k int, v int) USING flux;
  CREATE TABLE cr_hp (id int, k int, v int) USING heap;
  CREATE INDEX cr_fx_k ON cr_fx (k);
  CREATE INDEX cr_hp_k ON cr_hp (k);
  INSERT INTO cr_fx SELECT g, g, 0 FROM generate_series(1, 5) g;
  INSERT INTO cr_hp SELECT g, g, 0 FROM generate_series(1, 5) g;
}
teardown
{
  DROP TABLE IF EXISTS kw, cr_fx, cr_hp;
  DROP EXTENSION amcheck;
  DROP EXTENSION injection_points;
}

session s1
step pick_fx	{ ALTER TABLE cr_fx RENAME TO kw; ALTER INDEX cr_fx_k RENAME TO kw_k; }
step pick_hp	{ ALTER TABLE cr_hp RENAME TO kw; ALTER INDEX cr_hp_k RENAME TO kw_k; }
step s1b	{ BEGIN; }
step s1k	{ UPDATE kw SET k = 100 WHERE id = 3; }
step s1r	{ ROLLBACK; }
step s1c	{ COMMIT; }
step back	{ UPDATE kw SET k = 3 WHERE id = 3; }

session s2
setup	{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('flux-update-after-decision', 'wait');
}
step s2k	{ UPDATE kw SET k = 100, v = 2 WHERE id = 3 RETURNING id, k, v; }
step s2k2	{ UPDATE kw SET k = 200, v = 2 WHERE id = 3 RETURNING id, k, v; }
step s2done	{ }

session s3
step wake	{
  DO $$
  BEGIN
    PERFORM injection_points_detach('flux-update-after-decision');
    IF EXISTS (SELECT FROM pg_stat_activity
                WHERE wait_event = 'flux-update-after-decision') THEN
      PERFORM injection_points_wakeup('flux-update-after-decision');
    END IF;
  END $$;
}
step chk	{
  SET enable_seqscan = off; SET enable_bitmapscan = off;
  SET enable_indexonlyscan = off;
  SELECT 'idx' AS via, id, k, v FROM kw WHERE k IN (3, 100, 200) ORDER BY k;
  RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexonlyscan;
  SELECT 'seq' AS via, id, k, v FROM kw WHERE id = 3;
  SELECT bt_index_parent_check('kw_k', true, true);
}

# CAS: s2 decides from s1's image.
permutation pick_fx s1b s1k s2k s1r wake s2done chk
permutation pick_hp s1b s1k s2k s1r wake s2done chk
permutation pick_fx s1b s1k s2k s1c wake s2done chk
permutation pick_hp s1b s1k s2k s1c wake s2done chk
# Copy of the old row: s1 writes after s2's decision.
permutation pick_fx s2k2 s1b s1k wake s1r s2done back chk
permutation pick_hp s2k2 s1b s1k wake s1r s2done back chk
