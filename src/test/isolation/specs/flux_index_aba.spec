# FLUX: an old snapshot still reaches a row by its old key after a later
# key UPDATE of the row is rolled back.
#
# A FLUX UPDATE of an indexed column keeps the row at its TID: it inserts or
# revives (k_new, TID) and delete-marks (k_old, TID), which an older snapshot
# needs to find the row by k_old.  s1's REPEATABLE READ snapshot predates
# s2's committed A -> C, so it reads A through the delete-marked (A, TID).
# s2 then moves the row back to A, reviving (A, TID), and rolls back.  The
# rollback must leave (A, TID) delete-marked, not remove it: s1's index scan
# for A must still return the row, as its seqscan does.
#
# fx is FLUX, hp its heap twin, which gets the same statements; heap never
# reuses a TID, so it is the control.  Every query reads both tables, by
# index scan and by seqscan, and must return the same rows from all four.

setup
{
  CREATE TABLE fx (id int PRIMARY KEY, k int) USING flux;
  CREATE TABLE hp (id int PRIMARY KEY, k int) USING heap;
  INSERT INTO fx SELECT g, g FROM generate_series(1, 20) g;
  INSERT INTO hp SELECT g, g FROM generate_series(1, 20) g;
  CREATE INDEX fx_k ON fx (k);
  CREATE INDEX hp_k ON hp (k);

  -- the rows each table returns for k = key, by index scan and by seqscan
  CREATE FUNCTION aba_rows(key int) RETURNS TABLE (via text, id int, k int)
  LANGUAGE plpgsql AS $$
  BEGIN
    PERFORM set_config('enable_seqscan', 'off', true);
    PERFORM set_config('enable_bitmapscan', 'off', true);
    PERFORM set_config('enable_indexonlyscan', 'off', true);
    RETURN QUERY SELECT 'fx-idx', fx.id, fx.k FROM fx WHERE fx.k = key;
    RETURN QUERY SELECT 'hp-idx', hp.id, hp.k FROM hp WHERE hp.k = key;
    PERFORM set_config('enable_seqscan', 'on', true);
    PERFORM set_config('enable_indexscan', 'off', true);
    RETURN QUERY SELECT 'fx-seq', fx.id, fx.k FROM fx WHERE fx.k = key;
    RETURN QUERY SELECT 'hp-seq', hp.id, hp.k FROM hp WHERE hp.k = key;
    PERFORM set_config('enable_indexscan', 'on', true);
    PERFORM set_config('enable_bitmapscan', 'on', true);
    PERFORM set_config('enable_indexonlyscan', 'on', true);
  END $$;
}

teardown
{
  DROP TABLE fx, hp;
  DROP FUNCTION aba_rows;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT k FROM fx WHERE id = 12; }
step s1a	{ SELECT * FROM aba_rows(12); }
step s1c	{ SELECT * FROM aba_rows(3000012); }
step s1e	{ COMMIT; }

session s2
# committed A -> C
step s2c	{ UPDATE fx SET k = 3000012 WHERE id = 12; UPDATE hp SET k = 3000012 WHERE id = 12; }
# C -> A, rolled back
step s2a	{ BEGIN; UPDATE fx SET k = 12 WHERE id = 12; UPDATE hp SET k = 12 WHERE id = 12; ROLLBACK; }
# C -> A -> C -> A, rolled back
step s2x	{ BEGIN;
			  UPDATE fx SET k = 12 WHERE id = 12; UPDATE hp SET k = 12 WHERE id = 12;
			  UPDATE fx SET k = 3000012 WHERE id = 12; UPDATE hp SET k = 3000012 WHERE id = 12;
			  UPDATE fx SET k = 12 WHERE id = 12; UPDATE hp SET k = 12 WHERE id = 12;
			  ROLLBACK; }
# A -> B -> A, rolled back (no committed change: s1 and new snapshots agree)
step s2b	{ BEGIN;
			  UPDATE fx SET k = 4000012 WHERE id = 12; UPDATE hp SET k = 4000012 WHERE id = 12;
			  UPDATE fx SET k = 12 WHERE id = 12; UPDATE hp SET k = 12 WHERE id = 12;
			  ROLLBACK; }
# what a new snapshot sees
step s2r	{ SELECT * FROM aba_rows(12) UNION ALL SELECT * FROM aba_rows(3000012); }

permutation s1b s2c s2a s1a s1c s1e s2r
permutation s1b s2c s2x s1a s1c s1e s2r
permutation s1b s2b s1a s1e s2r
