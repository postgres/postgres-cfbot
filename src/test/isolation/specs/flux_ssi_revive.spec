# SSI rw-conflict-in on an index entry revived by an in-place key UPDATE.
# heap_ssi_revive.spec runs the same steps on heap; the two expected
# outputs are identical.
#
# s3 (read committed) moves row 1 to k = 1 and away again, so FLUX keeps a
# delete-marked (1, TID) entry in t_k.  When s2 moves the row back to k = 1,
# FLUX revives that entry instead of inserting a new one.  An insert reports
# a rw-conflict to every serializable reader holding a predicate lock on the
# leaf page it lands on, which is what protects a key-range read from a
# phantom; the revive must report it the same way.
#
# Permutations 1-2: s1 reads k = 1 before the tombstone exists, so it holds
# only the leaf page lock; s1 and s2 then form a write skew through u, and
# one of them must fail.  Permutation 3: the tombstone exists when s1 reads
# k = 1, so s1 also locks row 1 through it.  Permutation 4: s1 reads another
# key on the same leaf; heap reports the (false positive) conflict through
# the page lock, and FLUX does the same.

setup
{
  CREATE TABLE t (id int PRIMARY KEY, k int, v int) USING flux;
  CREATE INDEX t_k ON t (k);
  INSERT INTO t VALUES (1, 3, 0), (2, 5, 0), (3, 7, 0);
  CREATE TABLE u (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO u VALUES (1, 0);
}

teardown
{
  DROP TABLE t, u;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s1r	{ SELECT count(*) FROM t WHERE k = 1; }
step s1r5	{ SELECT count(*) FROM t WHERE k = 5; }
step s1w	{ UPDATE u SET v = v + 1 WHERE id = 1; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL SERIALIZABLE; SET LOCAL enable_seqscan = off; SET LOCAL enable_bitmapscan = off; }
step s2r	{ SELECT v FROM u WHERE id = 1; }
step s2w	{ UPDATE t SET k = 1 WHERE id = 1; }
step s2c	{ COMMIT; }

session s3
step s3a	{ UPDATE t SET k = 1 WHERE id = 1; }
step s3b	{ UPDATE t SET k = 2 WHERE id = 1; }
step s3chk	{ SELECT id, k FROM t ORDER BY id; SELECT * FROM u; }

permutation s1b s1r s3a s3b s2b s2r s2w s1w s1c s2c s3chk
permutation s1b s1r s3a s3b s2b s2r s2w s2c s1w s1c s3chk
permutation s3a s3b s1b s1r s2b s2r s2w s1w s1c s2c s3chk
permutation s3a s3b s1b s1r5 s2b s2r s2w s1w s1c s2c s3chk
