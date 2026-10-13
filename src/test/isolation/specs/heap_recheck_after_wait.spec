# heap control for flux_recheck_after_wait.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE rw (id int PRIMARY KEY, v int) USING heap;
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
