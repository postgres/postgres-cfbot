# FLUX: concurrent UPDATE serialization must match heap, and a plain UPDATE
# must not hold a heavyweight tuple lock until commit.
#
# FLUX updates rows in place at a stable TID.  The in-progress updater is the
# on-page t_xmin of the new image (FLUX_TUPLE_UPDATED | FLUX_TUPLE_UNCOMMITTED),
# which is what a concurrent updater must wait on, exactly as heap waits on
# xmax.  The expected output is heap's (the same spec with USING heap).

setup
{
  CREATE TABLE fus (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO fus VALUES (1, 0);
  CREATE TABLE fus_big (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO fus_big SELECT g, 0 FROM generate_series(1, 20000) g;
}

teardown
{
  DROP TABLE fus, fus_big;
}

session s1
step s1rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE fus SET v = v + 1 WHERE id = 1; }
step s1ks	{ SELECT id FROM fus WHERE id = 1 FOR KEY SHARE; }
step s1fu	{ SELECT id FROM fus WHERE id = 1 FOR UPDATE; }
step s1oc	{ INSERT INTO fus VALUES (1, 0) ON CONFLICT (id) DO UPDATE SET v = fus.v + 1; }
step s1c	{ COMMIT; }
step s1a	{ ROLLBACK; }
# A whole-table UPDATE must not take one transaction-duration lock per row:
# 20000 rows exhaust the default lock table ("out of shared memory").
step s1big	{ UPDATE fus_big SET v = v + 1; }
step s1bigsum	{ SELECT sum(v) FROM fus_big; }
# No tuple lock may survive the UPDATE statement (heap holds none).
step s1locks	{ SELECT count(*) AS tuple_locks FROM pg_locks WHERE locktype = 'tuple' AND pid = pg_backend_pid(); }

session s2
step s2rc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2snap	{ SELECT v FROM fus WHERE id = 1; }
step s2u	{ UPDATE fus SET v = v + 1 WHERE id = 1; }
step s2oc	{ INSERT INTO fus VALUES (1, 0) ON CONFLICT (id) DO UPDATE SET v = fus.v + 1; }
step s2c	{ COMMIT; }

session s3
step s3r	{ SELECT v FROM fus WHERE id = 1; }

# READ COMMITTED: s2 waits for s1, then re-evaluates on s1's value -> 2.
permutation s1rc s2rc s1u s2u s1c s2c s3r
# READ COMMITTED, s1 rolls back: s2 proceeds on the original value -> 1.
permutation s1rc s2rc s1u s2u s1a s2c s3r
# REPEATABLE READ, s1 commits while s2 waits: serialization failure -> 1.
permutation s1rc s2rr s2snap s1u s2u s1c s2c s3r
# REPEATABLE READ, s1 rolls back while s2 waits: s2 succeeds -> 1.
permutation s1rc s2rr s2snap s1u s2u s1a s2c s3r
# REPEATABLE READ, s1 committed after s2's snapshot: immediate failure -> 1.
permutation s1rc s2rr s2snap s1u s1c s2u s2c s3r
# s1 holds only a KEY SHARE lock, so s2's no-key UPDATE proceeds; s1's own
# later UPDATE must then wait for s2 and re-evaluate (no lost update) -> 2.
permutation s1rc s2rc s1ks s2u s1u s2c s1c s3r
# FOR UPDATE blocks a concurrent UPDATE until commit -> 2.
permutation s1rc s2rc s1fu s2u s1u s1c s2c s3r
# Concurrent ON CONFLICT DO UPDATE on the same key -> 2.
permutation s1rc s2rc s1oc s2oc s1c s2c s3r
# Bulk UPDATE at default settings, then no tuple locks left in the xact.
permutation s1rc s1big s1locks s1c s1bigsum
