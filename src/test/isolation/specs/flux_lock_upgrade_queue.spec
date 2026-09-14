# A transaction that already holds a row lock and waits to strengthen it
# must not queue on the heavyweight tuple lock, as with heap: a waiter
# ahead of it in that queue may be waiting for the lock it already holds
# (heap_lock_tuple's skip_tuple_lock, heap_delete's current_is_member).
# heap_lock_upgrade_queue.spec runs the same steps on heap; the two
# expected outputs are identical.
#
# FLUX used to queue the upgrader behind the waiter, which then waited for
# the upgrader's own lock: "deadlock detected" here, and in
# tuplelock-upgrade-no-deadlock, where the cycle closes through an idle
# session, a wait that never ends.
setup
{
  CREATE TABLE luq (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO luq VALUES (1, 0);
}
teardown
{
  DROP TABLE luq;
}

session s1
setup		{ BEGIN; }
step s1ks	{ SELECT id FROM luq FOR KEY SHARE; }
step s1sh	{ SELECT id FROM luq FOR SHARE; }
step s1sp	{ SAVEPOINT a; }
step s1rs	{ ROLLBACK TO a; }
step s1r	{ ROLLBACK; }

session s2
setup		{ BEGIN; }
step s2ks	{ SELECT id FROM luq FOR KEY SHARE; }
step s2nk	{ SELECT id FROM luq FOR NO KEY UPDATE; }
step s2fu	{ SELECT id FROM luq FOR UPDATE; }
step s2c	{ COMMIT; }

session s3
setup		{ BEGIN; }
step s3ks	{ SELECT id FROM luq FOR KEY SHARE; }
step s3sh	{ SELECT id FROM luq FOR SHARE; }
step s3fu	{ SELECT id FROM luq FOR UPDATE; }
step s3d	{ DELETE FROM luq RETURNING id; }
step s3c	{ COMMIT; }

# s2 upgrades past a lock s1 took in a savepoint while s3 waits for s1: s2
# wakes at ROLLBACK TO, then s3 waits for s2.
permutation s1ks s3fu s2ks s1sp s1sh s2nk s1rs s1r s2c s3c
# s3 upgrades its SHARE lock while s2 waits for s1.
permutation s1sh s2fu s3sh s3fu s1r s3c s2c
# s3 deletes the row it holds a KEY SHARE lock on while s2 waits for s1.
permutation s1ks s2fu s3ks s3d s1r s3c s2c
