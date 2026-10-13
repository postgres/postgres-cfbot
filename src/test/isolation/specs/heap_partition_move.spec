# heap control for flux_partition_move.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE pm (a int, b text) PARTITION BY LIST (a);
  CREATE TABLE pm1 PARTITION OF pm FOR VALUES IN (1) USING heap;
  CREATE TABLE pm2 PARTITION OF pm FOR VALUES IN (2) USING heap;
  INSERT INTO pm VALUES (1, 'ABC');
  CREATE TABLE pr (a int, b text) PARTITION BY RANGE (a);
  CREATE TABLE pr1 PARTITION OF pr FOR VALUES FROM (1) TO (10) USING heap;
  CREATE TABLE pr2 PARTITION OF pr FOR VALUES FROM (10) TO (20) USING heap;
  INSERT INTO pr VALUES (7, 'ABC');
  CREATE UNIQUE INDEX pr1_a ON pr1 (a);
  CREATE TABLE prc (a int REFERENCES pr1 (a)) USING heap;
}
teardown
{
  DROP TABLE prc, pr, pm;
}

session s1
setup		{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1u	{ UPDATE pm SET a = 2 WHERE a = 1; }
step s1sp	{ SAVEPOINT sp; }
step s1rs	{ ROLLBACK TO sp; }
step s1upr	{ UPDATE pr SET a = 11 WHERE a = 7; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2sel	{ SELECT * FROM pm ORDER BY a; }
step s2u	{ UPDATE pm SET b = 'EFG' WHERE a = 1; }
step s2d	{ DELETE FROM pm WHERE a = 1; }
step s2l	{ SELECT * FROM pm WHERE a = 1 FOR UPDATE; }
step s2ks	{ SELECT * FROM pm WHERE a = 1 FOR KEY SHARE; }
step s2m	{ MERGE INTO pm t USING (SELECT 1 AS a) s ON t.a = s.a
			  WHEN MATCHED THEN UPDATE SET b = 'merged'; }
step s2i	{ INSERT INTO prc VALUES (7); }
step s2c	{ COMMIT; }

# The move commits: each write or lock of the old row fails.
permutation s2b s1u s2d s1c s2sel s2c
permutation s2b s1u s2u s1c s2sel s2c
permutation s2b s1u s2l s1c s2c
permutation s2b s1u s2ks s1c s2c
permutation s2b s1u s2m s1c s2sel s2c
permutation s2b s1upr s2i s1c s2c
# Under REPEATABLE READ the row was concurrently updated.
permutation s2brr s2sel s1u s1c s2d s2c
permutation s2brr s2sel s1u s1c s2l s2c
# The move rolls back: the old row is still there, lockable and updatable.
permutation s2b s1u s2u s1r s2sel s2c
permutation s2b s1u s2d s1r s2sel s2c
permutation s2b s1u s2l s1r s2c
permutation s2b s1upr s2i s1r s2c
permutation s1sp s1u s1rs s2b s2u s2sel s2c s1c
permutation s2b s1u s1r s2l s2u s2sel s2c
