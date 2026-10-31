# heap control for flux_tid_fetch_snapshot.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE acc (id text PRIMARY KEY, bal int) USING heap;
  INSERT INTO acc VALUES ('checking', 600), ('savings', 600);
  CREATE TABLE oth (id text, v int) USING heap;
  INSERT INTO oth VALUES ('checking', 1), ('savings', 1);
}
teardown
{
  DROP TABLE acc, oth;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1sel	{ SELECT * FROM acc ORDER BY id; }
step s1lock	{ SELECT * FROM acc a1, acc a2 WHERE a1.id = a2.id ORDER BY a1.id FOR UPDATE OF a1; }
step s1tid	{ SELECT * FROM acc WHERE ctid = '(0,1)'; }
step s1join	{ SELECT * FROM acc a JOIN oth o USING (id) ORDER BY id FOR UPDATE OF a; }
step s1c	{ COMMIT; }

session s2
step s2u	{ UPDATE acc SET bal = bal + 450 WHERE id = 'checking'; }
step s2d	{ DELETE FROM acc WHERE id = 'checking'; }
step s2uo	{ UPDATE oth SET v = v + 10 WHERE id = 'checking'; }
step s2do	{ DELETE FROM oth WHERE id = 'checking'; }
step s2b	{ BEGIN; }
step s2c	{ COMMIT; }

# The recheck sees a2 as the query did: 600, beside a1's new 1050.
permutation s2b s2u s1b s1lock s2c s1c
# The unlocked side is another table, updated or deleted concurrently.
permutation s2b s2u s2uo s1b s1join s2c s1c
permutation s2b s2u s2do s1b s1join s2c s1c
# A TID scan sees the version its snapshot sees, updated or deleted since.
permutation s1brr s1sel s2u s1tid s1c
permutation s1brr s1sel s2d s1tid s1c
permutation s1brr s1sel s2u s2u s1tid s1c
