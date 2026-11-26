# Test for write skew under SERIALIZABLE with a bitmap heap scan

setup
{
  CREATE TABLE test (i int PRIMARY KEY, t text);
  INSERT INTO test VALUES (5, 'apple'), (7, 'pear'), (11, 'banana');
}

teardown
{
  DROP TABLE test;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET enable_seqscan = off;
  SET enable_indexscan = off;
  SET enable_bitmapscan = on;
}
step r1	{ SELECT * FROM test WHERE i IN (5, 7); }
step w1	{ UPDATE test SET t = 'pear_xact1' WHERE i = 7; }
step c1	{ COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET enable_seqscan = off;
  SET enable_indexscan = off;
  SET enable_bitmapscan = on;
}
step r2	{ SELECT * FROM test WHERE i IN (5, 7); }
step w2	{ UPDATE test SET t = 'apple_xact2' WHERE i = 5; }
step c2	{ COMMIT; }

permutation r1 r2 w1 w2 c1 c2
permutation r2 r1 w2 w1 c2 c1
