# A waiter blocked by a row lock taken in a savepoint must wake when the
# savepoint is rolled back, as with heap, whose locker is the
# subtransaction's xid.  heap_savepoint_lock_wake.spec runs the same steps
# on heap; the two expected outputs are identical.
#
# FLUX used to wait on the locker's top-level xid, so the waiter stayed
# blocked after ROLLBACK TO had released the stronger lock, until the
# locker's transaction ended.
setup
{
  CREATE TABLE lw (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO lw VALUES (1, 0);
}
teardown
{
  DROP TABLE lw;
}

session s1
setup		{ BEGIN; }
step s1ks	{ SELECT * FROM lw FOR KEY SHARE; }
step s1sp	{ SAVEPOINT a; }
step s1sp2	{ SAVEPOINT b; }
step s1nk	{ SELECT * FROM lw FOR NO KEY UPDATE; }
step s1fu	{ SELECT * FROM lw FOR UPDATE; }
step s1rs	{ ROLLBACK TO a; }
step s1rs2	{ ROLLBACK TO b; }
step s1rel2	{ RELEASE b; }
step s1c	{ COMMIT; }

session s2
setup		{ BEGIN; }
step s2nk	{ SELECT * FROM lw FOR NO KEY UPDATE; }
step s2fs	{ SELECT * FROM lw FOR SHARE; }
step s2u	{ UPDATE lw SET v = v + 1 RETURNING *; }
step s2d	{ DELETE FROM lw RETURNING *; }
step s2c	{ COMMIT; }

# The waiter wakes at ROLLBACK TO: a lock upgrade, and a lock with nothing
# at the top level.
permutation s1ks s1sp s1nk s2nk s1rs s1c s2c
permutation s1ks s1sp s1nk s2u s1rs s1c s2c
permutation s1ks s1sp s1fu s2fs s1rs s1c s2c
permutation s1sp s1fu s2u s1rs s1c s2c
# Nested savepoints: the inner lock released into the outer savepoint,
# which is then rolled back; and only the inner savepoint rolled back.
permutation s1ks s1sp s1sp2 s1nk s1rel2 s2nk s1rs s1c s2c
permutation s1ks s1sp s1sp2 s1nk s2nk s1rs2 s1c s2c
# The top-level KEY SHARE lock still blocks a DELETE until commit.
permutation s1ks s1sp s1nk s2d s1rs s1c s2c
