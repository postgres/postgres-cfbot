# An UPDATE that conflicts with several row lockers must wait for all of
# them: two sessions hold FOR SHARE on the row, the first rolls back, and the
# UPDATE must keep waiting for the second instead of overwriting the row it
# still has locked.  HEAP twin; the expected output is identical.
setup
{
	CREATE TABLE lu (id int PRIMARY KEY, v int) USING heap;
	INSERT INTO lu VALUES (1, 0);
}

teardown
{
	DROP TABLE lu;
}

session s1
step s1b	{ BEGIN; }
step s1l	{ SELECT v FROM lu WHERE id = 1 FOR SHARE; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN; }
step s2l	{ SELECT v FROM lu WHERE id = 1 FOR SHARE; }
step s2chk	{ SELECT v FROM lu WHERE id = 1; }
step s2c	{ COMMIT; }

session s3
step s3u	{ UPDATE lu SET v = v + 1 WHERE id = 1; }
step s3chk	{ SELECT v FROM lu WHERE id = 1; }

# s3 must still be waiting after s1 rolls back; s2 still sees v = 0.
permutation s1b s1l s2b s2l s3u s1r s2chk s2c s3chk
