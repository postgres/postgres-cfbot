# heap control for flux_update_waiter_abort.spec: same steps, same expected
# output.  A row change that waits for a concurrent writer of the same row
# works on the committed new version if the writer commits, and on the version
# the writer replaced if it aborts.
setup
{
  CREATE TABLE vh (id int PRIMARY KEY, v int, w int) USING heap;
  INSERT INTO vh VALUES (1, 0, 0), (2, 0, 0);
  CREATE TABLE vh_hist (id int);
}
teardown
{
  DROP TABLE vh, vh_hist;
}
session s1
setup		{ SET deadlock_timeout = '10ms'; }
step s1b	{ BEGIN; }
step s1w	{ UPDATE vh SET w = 10 WHERE id = 1; }
step s1w2	{ UPDATE vh SET w = 20 WHERE id = 1; }
step s1k	{ UPDATE vh SET id = 10 WHERE id = 1; }
step s1sp	{ SAVEPOINT a; }
step s1rs	{ ROLLBACK TO a; }
step s1u1	{ UPDATE vh SET v = v + 1 WHERE id = 1; INSERT INTO vh_hist VALUES (1); }
step s1u2	{ UPDATE vh SET v = v + 1 WHERE id = 2; INSERT INTO vh_hist VALUES (2); }
step s1r	{ ROLLBACK; }
step s1c	{ COMMIT; }
session s2
setup		{ SET deadlock_timeout = '10s'; }
step s2b	{ BEGIN; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2u	{ UPDATE vh SET v = v + 1 WHERE id = 1 RETURNING old.v, old.w, new.v, new.w; }
step s2w5	{ UPDATE vh SET w = 5 WHERE id = 1; }
step s2d	{ DELETE FROM vh WHERE id = 1 RETURNING *; }
step s2l	{ SELECT * FROM vh WHERE id = 1 FOR UPDATE; }
step s2h	{ INSERT INTO vh_hist VALUES (1); }
step s2c	{ COMMIT; }
session s3
setup		{ SET deadlock_timeout = '10s'; }
step s3b	{ BEGIN; }
step s3u	{ UPDATE vh SET v = v + 10 WHERE id = 1 RETURNING old.v, old.w, new.v, new.w; }
step s3u2	{ UPDATE vh SET v = v + 1 WHERE id = 2; INSERT INTO vh_hist VALUES (2); }
step s3u1	{ UPDATE vh SET v = v + 1 WHERE id = 1; INSERT INTO vh_hist VALUES (1); }
step s3c	{ COMMIT; }
session s4
step chk	{ SELECT * FROM vh ORDER BY id; }
step chkh	{ SELECT f.id, f.v, f.w, count(h.id) AS committed FROM vh f LEFT JOIN vh_hist h USING (id) GROUP BY f.id, f.v, f.w ORDER BY f.id; }

# READ COMMITTED UPDATE waiter: the writer commits (re-fetch: OLD w = 10),
# or aborts (OLD w = 0).
permutation s1b s1w s2b s2u s1c s2c chk
permutation s1b s1w s2b s2u s1r s2c chk
# REPEATABLE READ: commit is a serialization failure, abort succeeds.
permutation s1b s1w s2rr s2u s1c s2c chk
permutation s1b s1w s2rr s2u s1r s2c chk
# The writer's change is in a savepoint rolled back while the update waits;
# then with an earlier change outside the savepoint, which commits.
permutation s1b s1sp s1w s2b s2u s1rs s1c s2c chk
permutation s1b s1w s1sp s1w2 s2b s2u s1rs s1c s2c chk
# DELETE ... RETURNING waiters, and a SELECT ... FOR UPDATE waiter whose
# writer aborts.  (FOR UPDATE behind a writer that commits is
# flux_lock_waiter_commit.)
permutation s1b s1w s2b s2d s1c s2c chk
permutation s1b s1w s2b s2d s1r s2c chk
permutation s1b s1w s2b s2l s1r s2c chk
# Two waiters queued behind the writer.
permutation s1b s1w s2b s3b s2u s3u s1c s2c s3c chk
permutation s1b s1w s2b s3b s2u s3u s1r s2c s3c chk
# The writer changes the column the waiter's WHERE clause tests.
permutation s1b s1k s2b s2u s1c s2c chk
permutation s1b s1k s2b s2u s1r s2c chk
# The on-page version is our own earlier update.
permutation s2b s2w5 s2u s2c chk
# s1->s3 (row 2), s3->s2 (row 1, queued), s2->s1 (row 1): s1 is the deadlock
# victim; s2 must see OLD v = 0, not the victim's 1.
permutation s1b s2b s3b s1u1 s3u2 s2u(s1u2) s3u1 s1u2(*) s1c s2h s2c s3c chkh
