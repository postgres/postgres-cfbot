# An UPDATE that waits for a concurrent writer of the same row must decide
# whether it changes an indexed column, and which index entries to change,
# from the row the writer leaves behind, not from the writer's uncommitted
# image it found before the wait.  heap_update_key_waiter.spec runs the same
# steps on heap; the two expected outputs are identical.
#
# FLUX updates in place, so while the writer runs the page holds its
# uncommitted image.  An UPDATE that set k to the value the writer had set
# compared its new row with that image, found no indexed column changed, and
# after the writer aborted overwrote the restored row in place without index
# maintenance: the row then had the new k but the index had only its old one.
# Each permutation reads the row through the index under every key it may
# have, and once by a sequential scan; the two must agree.
setup
{
  CREATE TABLE kw (id int, k int, v int) USING flux;
  CREATE INDEX kw_k ON kw (k);
  INSERT INTO kw SELECT g, g, 0 FROM generate_series(1, 5) g;
}
teardown
{
  DROP TABLE kw;
}
session s1
step s1b	{ BEGIN; }
step s1k	{ UPDATE kw SET k = 100 WHERE id = 3; }
step s1v	{ UPDATE kw SET v = 1 WHERE id = 3; }
step s1d	{ DELETE FROM kw WHERE id = 3; }
step s1r	{ ROLLBACK; }
step s1c	{ COMMIT; }
session s2
step s2b	{ BEGIN; }
step s2k	{ UPDATE kw SET k = 100 WHERE id = 3; }
step s2k2	{ UPDATE kw SET k = 200 WHERE id = 3; }
step s2v	{ UPDATE kw SET v = 2 WHERE id = 3; }
step s2c	{ COMMIT; }
session s3
step chk	{
  SET enable_seqscan = off; SET enable_bitmapscan = off;
  SET enable_indexonlyscan = off;
  SELECT id, k, v FROM kw WHERE k IN (3, 100, 200) ORDER BY k;
  RESET enable_seqscan; RESET enable_bitmapscan; RESET enable_indexonlyscan;
  SELECT id, k, v FROM kw WHERE id = 3;
}

# The waiter's new key equals the aborted writer's key.
permutation s1b s1k s2b s2k s1r s2c chk
permutation s1b s1k s2b s2k s1c s2c chk
# The waiter changes the key to another value.
permutation s1b s1k s2b s2k2 s1r s2c chk
permutation s1b s1k s2b s2k2 s1c s2c chk
# The writer changed the key, the waiter changes only a non-key column.
permutation s1b s1k s2b s2v s1r s2c chk
permutation s1b s1k s2b s2v s1c s2c chk
# The writer changed a non-key column, the waiter changes the key.
permutation s1b s1v s2b s2k s1r s2c chk
permutation s1b s1v s2b s2k s1c s2c chk
# The writer deleted the row and rolls back.
permutation s1b s1d s2b s2k s1r s2c chk
permutation s1b s1d s2b s2k s1c s2c chk
