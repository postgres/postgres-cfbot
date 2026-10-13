# heap control for flux_outofplace_grow.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE xg (id int, n int, p1 bigint, p2 bigint, p3 bigint, p4 bigint,
                   p5 bigint, p6 bigint, p7 bigint, p8 bigint)
    USING heap WITH (fillfactor = 100);
  INSERT INTO xg (id, n) SELECT g, 0 FROM generate_series(1, 400) g;
}
teardown
{
  DROP TABLE xg;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1g	{ UPDATE xg SET p1 = 1, p2 = 2, p3 = 3, p4 = 4, p5 = 5, p6 = 6,
			  p7 = 7, p8 = 8 WHERE id = 1; }
step s1c	{ COMMIT; }
step s1r	{ ROLLBACK; }
step s1sel	{ SELECT id, n, p8, (SELECT count(*) FROM xg) AS nrows
			  FROM xg WHERE id = 1; }

session s2
step s2b	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2u	{ UPDATE xg SET n = n + 1 WHERE id = 1; }
step s2c	{ COMMIT; }

permutation s1b s2b s1g s2u s1c s2c s1sel
permutation s1b s2b s1g s2u s1r s2c s1sel
