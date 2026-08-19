# Test that a serialization failure raised inside a subtransaction cannot be
# discarded by rolling back the subtransaction: the reads that triggered it
# still happened, so the top level transaction must be doomed.
#
# s1 and s2 form the classic write-skew dangerous structure under SERIALIZABLE:
#   s1 reads row 2 and writes row 1
#   s2 writes row 2 and reads row 1
# s1 commits first.  When s2 then reads row 1 it is correctly identified as the
# pivot of a dangerous structure and PostgreSQL raises
#   ERROR:  could not serialize access ...
#   (Canceled on conflict out to pivot ..., during read).
#
# Crucially, s2's *write* to row 2 happened BEFORE the savepoint, so it is not
# undone.  s2 only wraps the offending READ in a SAVEPOINT, rolls back to it
# (swallowing the error) and commits.  That COMMIT must fail: allowing it
# leaves both s1's and s2's writes committed, which is the write-skew anomaly
# SSI is supposed to prevent (no serial order exists).

setup
{
  CREATE TABLE t (id int PRIMARY KEY, v int);
  INSERT INTO t VALUES (1, 0), (2, 0), (3, 0);
}

teardown
{
  DROP TABLE t;
}

session s1
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r1  { SELECT v FROM t WHERE id = 2; }
step w1  { UPDATE t SET v = 1 WHERE id = 1; }
step c1  { COMMIT; }

session s2
setup { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step w2   { UPDATE t SET v = 1 WHERE id = 2; }
step r2a  { SELECT v FROM t WHERE id = 1; }
step sp2  { SAVEPOINT f; }
step r2   { SELECT v FROM t WHERE id = 1; }
step r2u  { SELECT v FROM t WHERE id = 3; }
step r2x  { DO $$
            BEGIN
              PERFORM v FROM t WHERE id = 1;
            EXCEPTION WHEN serialization_failure THEN
              RAISE NOTICE 'serialization failure swallowed';
            END $$; }
step w2b  { UPDATE t SET v = 2 WHERE id = 2; }
step rb2  { ROLLBACK TO SAVEPOINT f; }
step c2   { COMMIT; }

# Used to observe the final committed state.
session s3
step rall { SELECT id, v FROM t ORDER BY id; }

# s2 takes its snapshot at w2 (before s1 commits), writes row 2, then after s1
# commits it reads row 1 inside a savepoint and is cancelled.  After rolling
# back to the savepoint it must not be able to commit.  If it does (the bug),
# rall shows both rows updated -- the non-serializable write-skew outcome.
permutation r1 w2 w1 c1 sp2 r2 rb2 c2 rall

# Same dangerous structure, but the failing read runs inside a PL/pgSQL
# exception block, which uses a subtransaction just like a savepoint does.
# Swallowing the error must not allow the COMMIT.
permutation r1 w2 w1 c1 r2x c2 rall

# Here the serialization failure is raised during a *write*: s2 is identified
# as a pivot when it updates row 2, which s1 read.  The write itself is undone
# by the rollback to the savepoint, but the transaction is doomed anyway: when
# the failure is detected there is no way to know that the subtransaction will
# roll back, and a transaction that has swallowed a serialization failure
# cannot be assumed safe to commit.  This permutation documents that
# conservative behavior.
permutation r2a r1 w1 c1 sp2 w2b rb2 c2 rall

# A transaction that has swallowed a serialization failure stays doomed: the
# next read re-reports the failure immediately, it does not take until COMMIT.
# The read is of row 3, which nobody else touched, so it is only cancelled
# because the transaction is doomed, not by rediscovering the dangerous
# structure.
permutation r1 w2 w1 c1 sp2 r2 rb2 r2u c2 rall

# ... and so does the next write.
permutation r1 w2 w1 c1 sp2 r2 rb2 w2b c2 rall
