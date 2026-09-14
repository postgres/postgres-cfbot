# A row lock or a row change made in a savepoint that is rolled back must
# leave nothing behind once the transaction commits.  heap_savepoint_lock_
# rollback.spec runs the same steps on heap; the two expected outputs are
# identical.
#
# FLUX used to mark the row DELETED when s1 committed, because s1 still had
# an ABORTED sLog op for it (the rolled-back lock), and the image on the page
# carried an UNCOMMITTED hint -- s2's, committed or still running.  The row
# stayed visible to a plain SELECT, but UPDATE, DELETE and row locks no
# longer found it.
setup
{
  CREATE TABLE lr (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO lr VALUES (1, 0), (2, 0);
}
setup
{
  VACUUM lr;
}
teardown
{
  DROP TABLE lr;
}

session s1
step s1b	{ BEGIN; }
step s1sp	{ SAVEPOINT a; }
step s1sp2	{ SAVEPOINT b; }
step s1fu	{ SELECT * FROM lr WHERE id = 1 FOR UPDATE; }
step s1fnk	{ SELECT * FROM lr WHERE id = 1 FOR NO KEY UPDATE; }
step s1fs	{ SELECT * FROM lr WHERE id = 1 FOR SHARE; }
step s1fks	{ SELECT * FROM lr WHERE id = 1 FOR KEY SHARE; }
step s1u	{ UPDATE lr SET v = v + 10 WHERE id = 1; }
step s1i	{ INSERT INTO lr VALUES (3, 0); }
step s1fu3	{ SELECT * FROM lr WHERE id = 3 FOR UPDATE; }
step s1u3	{ UPDATE lr SET v = v + 10 WHERE id = 3; }
step s1d	{ DELETE FROM lr WHERE id = 1; }
step s1rs	{ ROLLBACK TO a; }
step s1rs2	{ ROLLBACK TO b; }
step s1rel	{ RELEASE a; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN; }
step s2u	{ UPDATE lr SET v = v + 1 WHERE id = 1; }
step s2d	{ DELETE FROM lr WHERE id = 1; }
step s2c	{ COMMIT; }
step s2r	{ ROLLBACK; }

# Every way of reaching the rows must find the same rows.
session s3
step s3chk
{
  SELECT 'select' AS via, * FROM lr ORDER BY id;
  SELECT 'key share' AS via, * FROM lr ORDER BY id FOR KEY SHARE;
  SELECT 'for update' AS via, * FROM lr ORDER BY id FOR UPDATE;
  WITH u AS (UPDATE lr SET v = v + 100 RETURNING *)
    SELECT 'update' AS via, * FROM u ORDER BY id;
  DELETE FROM lr WHERE id = 1 RETURNING 'delete' AS via, *;
  SELECT 'after delete' AS via, * FROM lr ORDER BY id;
}

# Each lock strength, in a savepoint that is rolled back, then a committed
# UPDATE of the row by s2.
permutation s1b s1sp s1fu s1rs s2u s1c s3chk
permutation s1b s1sp s1fnk s1rs s2u s1c s3chk
permutation s1b s1sp s1fs s1rs s2u s1c s3chk
permutation s1b s1sp s1fks s1rs s2u s1c s3chk
# Nested savepoints: both rolled back; only the inner one, the outer
# released.
permutation s1b s1sp s1sp2 s1fu s1rs2 s1rs s2u s1c s3chk
permutation s1b s1sp s1sp2 s1fu s1rs2 s1rel s2u s1c s3chk
# RELEASE instead of ROLLBACK TO: the lock is s1's until it commits.
permutation s1b s1sp s1fu s1rel s2u s1c s3chk
# s2 deletes the row instead.
permutation s1b s1sp s1fu s1rs s2d s1c s3chk
# s2 aborts its UPDATE before s1 commits.
permutation s1b s1sp s1fu s1rs s2b s2u s2r s1c s3chk
# s2's UPDATE is still running when s1 commits.
permutation s1b s1sp s1fu s1rs s2b s2u s1c s2c s3chk
# s1 rolls back instead, with the lock in a savepoint or at the top level.
permutation s1b s1sp s1fu s1rs s2u s1r s3chk
permutation s1b s1fu s1r s2u s3chk
# A row change, not a lock, in the rolled-back savepoint, then s2's
# committed UPDATE.
permutation s1b s1sp s1u s1rs s2u s1c s3chk
permutation s1b s1sp s1d s1rs s2u s1c s3chk
# ... and s2's UPDATE still running when s1 commits.
permutation s1b s1sp s1u s1rs s2b s2u s1c s2c s3chk
permutation s1b s1sp s1d s1rs s2b s2u s1c s2c s3chk
# A top-level lock survives the same or a weaker lock in a rolled-back
# savepoint: s2 waits for s1.
permutation s1b s1fu s1sp s1fu s1rs s2u s1c s3chk
permutation s1b s1fu s1sp s1fks s1rs s2u s1c s3chk
permutation s1b s1fnk s1sp s1fu s1rs s2u s1c s3chk
# ... and a weaker lock at the same level does not give the stronger one up.
permutation s1b s1fu s1fks s2u s1c s3chk
permutation s1b s1fu s1sp s1fks s1rel s2u s1c s3chk
# ... nor does a row change in a rolled-back savepoint.
permutation s1b s1fu s1sp s1u s1rs s2u s1c s3chk
permutation s1b s1fks s1sp s1u s1rs s2d s1c s3chk
# s1's own top-level UPDATE of the row, then the rolled-back lock or
# UPDATE.  s1 still holds its UPDATE's row lock, so s2 waits for s1.
permutation s1b s1u s1sp s1fu s1rs s1c s3chk
permutation s1b s1u s1sp s1u s1rs s1c s3chk
permutation s1b s1u s1sp s1fu s1rs s2u s1c s3chk
permutation s1b s1u s1sp s1fks s1rs s2u s1r s3chk
# s1's own top-level INSERT, locked in a rolled-back savepoint: kept.  An
# INSERT in a rolled-back savepoint: discarded.
permutation s1b s1i s1sp s1fu3 s1rs s1c s3chk
permutation s1b s1sp s1i s1rs s1c s3chk
# An INSERT kept (top level, or in a released savepoint), then updated in a
# rolled-back savepoint: kept, with its first value.
permutation s1b s1i s1sp s1u3 s1rs s1c s3chk
permutation s1b s1sp s1i s1rel s1sp2 s1u3 s1rs2 s1c s3chk

# A rolled-back upgrade releases only the savepoint's stronger lock.
permutation s1b s1fks s1sp s1fu s1rs s2u s1c s3chk
permutation s1b s1fks s1sp s1sp2 s1fu s1rs2 s1rs s2u s1c s3chk
permutation s1b s1fks s1sp s1sp2 s1fu s1rel s2u s1c s3chk
