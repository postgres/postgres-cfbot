# heap control for flux_fk_crosscheck.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE pk (a int PRIMARY KEY) USING heap;
  CREATE TABLE fkc (a int REFERENCES pk ON DELETE CASCADE ON UPDATE CASCADE) USING heap;
  CREATE TABLE fkn (a int REFERENCES pk ON DELETE SET NULL) USING heap;
  INSERT INTO pk VALUES (2);
}
teardown
{
  DROP TABLE fkc, fkn, pk;
}

session s1
step s1brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s1ic	{ INSERT INTO fkc VALUES (2); }
step s1in	{ INSERT INTO fkn VALUES (2); }
step s1c	{ COMMIT; }
step s1sel	{ SELECT 'pk' t, a FROM pk UNION ALL SELECT 'fkc', a FROM fkc UNION ALL SELECT 'fkn', a FROM fkn ORDER BY 1, 2; }

session s2
step s2brr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; }
step s2brc	{ BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2d	{ DELETE FROM pk WHERE a = 2; }
step s2u	{ UPDATE pk SET a = 3 WHERE a = 2; }
step s2c	{ COMMIT; }

permutation s1brr s1ic s2brr s2d s1c s2c s1sel
permutation s1brr s1in s2brr s2d s1c s2c s1sel
permutation s1brr s1ic s2brr s2u s1c s2c s1sel
# READ COMMITTED has no crosscheck: the action applies to the new child.
permutation s1brr s1ic s2brc s2d s1c s2c s1sel
permutation s1brr s1in s2brc s2d s1c s2c s1sel
