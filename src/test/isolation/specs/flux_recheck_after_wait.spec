# A DELETE or UPDATE that waited for another transaction must classify the
# row again when it wakes, as heap does (heap_delete and heap_update start
# over): a row another writer has delete-marked in the meantime is deleted
# only once that writer commits, so it waits for it, and a row the waited-for
# transaction updated and committed goes to EvalPlanQual.
# heap_recheck_after_wait.spec runs the same steps on heap; the two expected
# outputs are identical.
#
# FLUX reported a delete-marked row as deleted at once, though its deleter
# was still in progress: the waiter returned no row, and when the deleter
# rolled back the row survived a DELETE, or lost an UPDATE.  And a DELETE
# that waited for a row lock deleted the row without rechecking it, after
# the locker had changed it so that it no longer matched.
setup
{
  CREATE TABLE rw (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO rw VALUES (1, 0);
}
teardown
{
  DROP TABLE rw;
}

session s1
setup		{ BEGIN; }
step s1sh	{ SELECT id FROM rw FOR SHARE; }
step s1u	{ UPDATE rw SET v = 5; }
step s1r	{ ROLLBACK; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2d	{ DELETE FROM rw RETURNING id, v; }
step s2r	{ ROLLBACK; }
step s2c	{ COMMIT; }

session s3
setup		{ BEGIN; }
step s3d	{ DELETE FROM rw WHERE v = 0 RETURNING id, v; }
step s3u	{ UPDATE rw SET v = v + 10 RETURNING id, v; }
step s3c	{ COMMIT; }

session s4
step s4sel	{ SELECT id, v FROM rw; }

# s2 waits for s1's row lock or update, s3 waits behind s2; s1 rolls back
# and s2 deletes.  s3 must wait for s2, which rolls back: s3 deletes or
# updates the row.
permutation s1sh s2d s3d s1r s2r s3c s4sel
permutation s1sh s2d s3u s1r s2r s3c s4sel
permutation s1u s2d s3d s1r s2r s3c s4sel
permutation s1u s2d s3u s1r s2r s3c s4sel
# s2 commits its delete: s3 finds no row.
permutation s1sh s2d s3d s1r s2c s3c s4sel
# s3 waits for s1's row lock; s1 updates the row so that it no longer
# matches s3's WHERE, and commits: s3 deletes nothing.
permutation s1sh s3d s1u s1c s3c s4sel
