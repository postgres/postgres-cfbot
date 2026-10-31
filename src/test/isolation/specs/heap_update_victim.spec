# heap control for flux_update_victim.spec: same steps, same expected output.
# FLUX: an UPDATE that waited for a row and then became a deadlock victim
# must not leave its increments behind.  Every value must equal the number
# of committed increments recorded in fdv_hist (same transaction), as in
# heap.

setup
{
  CREATE TABLE fdv (id int PRIMARY KEY, v int) USING heap;
  INSERT INTO fdv VALUES (1, 0), (2, 0);
  CREATE TABLE fdv_hist (id int);
}

teardown
{
  DROP TABLE fdv, fdv_hist;
}

session s1
setup		{ SET deadlock_timeout = '10s'; BEGIN; }
step s1u1	{ UPDATE fdv SET v = v + 1 WHERE id = 1; INSERT INTO fdv_hist VALUES (1); }
step s1u2	{ UPDATE fdv SET v = v + 1 WHERE id = 2; INSERT INTO fdv_hist VALUES (2); }
step s1c	{ COMMIT; }

session s2
setup		{ SET deadlock_timeout = '10ms'; BEGIN; }
step s2u1	{ UPDATE fdv SET v = v + 1 WHERE id = 1; INSERT INTO fdv_hist VALUES (1); }
step s2u2	{ UPDATE fdv SET v = v + 1 WHERE id = 2; INSERT INTO fdv_hist VALUES (2); }
step s2c	{ COMMIT; }

session s3
setup		{ SET deadlock_timeout = '10s'; BEGIN; }
step s3u1	{ UPDATE fdv SET v = v + 1 WHERE id = 1; INSERT INTO fdv_hist VALUES (1); }
step s3u2	{ UPDATE fdv SET v = v + 1 WHERE id = 2; INSERT INTO fdv_hist VALUES (2); }
step s3c	{ COMMIT; }

session s4
step chk	{ SELECT f.id, f.v, count(h.id) AS committed FROM fdv f LEFT JOIN fdv_hist h USING (id) GROUP BY f.id, f.v ORDER BY f.id; }

# Let s2 close the cycle, so its short timeout cannot expire before the
# deadlock exists.  Report its error before the released waiter completes.
permutation s2u2 s1u1 s1u2(s2u1) s2u1(*) s2c s1c s3c chk
# With s3 queued on row 2, it must commit before s1 can acquire that row.
permutation s2u2 s1u1 s3u2(s2u1) s1u2(s3c) s2u1(*) s2c s3c s1c chk
# s2's first update waits and succeeds; its second update closes a cycle
# with s3.  The failed transaction must undo the first increment too.
permutation s1u1 s3u2 s2u1 s1c s3u1(s2u2) s2u2(*) s2c s3c chk
