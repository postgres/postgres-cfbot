# A DELETE that conflicts with several row lockers must wait for all of them,
# not only the first: here two foreign-key checks hold FOR KEY SHARE on the
# parent row, and one of them rolls back.  The DELETE must still wait for the
# other, so its foreign-key check then finds the committed child row.
# FLUX twin of heap_lock_delete_lockers; the expected output is identical.
setup
{
	CREATE TABLE parent (i int PRIMARY KEY) USING flux;
	INSERT INTO parent VALUES (1);
	CREATE TABLE child (i int REFERENCES parent (i)) USING flux;
}

teardown
{
	DROP TABLE child, parent;
}

session s1
step s1b	{ BEGIN; }
step s1l	{ INSERT INTO child VALUES (1); }
step s1r	{ ROLLBACK; }
step s1c	{ COMMIT; }

session s2
step s2b	{ BEGIN; }
step s2l	{ INSERT INTO child VALUES (1); }
step s2c	{ COMMIT; }
step s2r	{ ROLLBACK; }

session s3
step s3d	{ DELETE FROM parent; }
step s3chk	{ SELECT (SELECT count(*) FROM parent) AS parents, (SELECT count(*) FROM child) AS children; }

# The first locker rolls back; the DELETE must keep waiting for the second.
permutation s1b s1l s2b s2l s3d s1r s2c s3chk
# The second locker rolls back first.
permutation s1b s1l s2b s2l s3d s2r s1c s3chk
# Both roll back: the DELETE goes ahead.
permutation s1b s1l s2b s2l s3d s1r s2r s3chk
