# heap control for flux_update_deadlock.spec: same steps, same expected output.
# FLUX: concurrent two-row UPDATE transactions that deadlock.  The deadlock
# victim's increments must be rolled back and every survivor's must land
# exactly once, as in heap.  hist records the increments of the transactions
# that commit, in the same transaction.

setup
{
  CREATE TABLE fdl (id int PRIMARY KEY, v int) USING heap;
  INSERT INTO fdl VALUES (1, 0), (2, 0);
  CREATE TABLE fdl_hist (id int, who text);
}

teardown
{
  DROP TABLE fdl, fdl_hist;
}

session s1
setup		{ SET deadlock_timeout = '10s'; BEGIN; }
step s1u1	{ UPDATE fdl SET v = v + 1 WHERE id = 1; INSERT INTO fdl_hist VALUES (1, 's1'); }
step s1u2	{ UPDATE fdl SET v = v + 1 WHERE id = 2; INSERT INTO fdl_hist VALUES (2, 's1'); }
step s1c	{ COMMIT; }

session s2
setup		{ SET deadlock_timeout = '500ms'; BEGIN; }
step s2u1	{ UPDATE fdl SET v = v + 1 WHERE id = 1; INSERT INTO fdl_hist VALUES (1, 's2'); }
step s2u2	{ UPDATE fdl SET v = v + 1 WHERE id = 2; INSERT INTO fdl_hist VALUES (2, 's2'); }
step s2c	{ COMMIT; }

session s3
setup		{ SET deadlock_timeout = '10s'; BEGIN; }
step s3u1	{ UPDATE fdl SET v = v + 1 WHERE id = 1; INSERT INTO fdl_hist VALUES (1, 's3'); }
step s3u2	{ UPDATE fdl SET v = v + 1 WHERE id = 2; INSERT INTO fdl_hist VALUES (2, 's3'); }
step s3c	{ COMMIT; }

session s4
step chk	{ SELECT id, v, (SELECT count(*) FROM fdl_hist h WHERE h.id = f.id) AS committed FROM fdl f ORDER BY id; }

# s2 runs with a deadlock_timeout far below the others' but far above the
# few milliseconds the tester needs to issue the step that closes the cycle,
# so s2 is always the one that detects it and is the victim.  Commits are
# ordered so that no session is asked to commit while it still waits.
#
# Two-way deadlock, s2 is the victim; s1 survives.
permutation s1u1 s2u2 s1u2 s2u1 s1c s2c s3c chk
# A third transaction waits on row 1 behind s1 before the cycle forms.
permutation s1u1 s2u2 s3u1 s1u2 s2u1 s1c s2c s3c chk
# The victim holds the row a waiter is queued on.
permutation s2u1 s1u2 s3u1 s2u2 s1u1 s3c s1c s2c chk
# Victim's row 2 has a queued waiter too.
permutation s1u1 s2u2 s3u2 s1u2 s2u1 s3c s1c s2c chk
