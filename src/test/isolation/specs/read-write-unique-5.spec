# Reusing a key that another transaction has deleted and committed, under a
# primary key constraint, with a plain insert and with ON CONFLICT DO NOTHING.

setup
{
  CREATE TABLE test (k int PRIMARY KEY, j int);
  INSERT INTO test VALUES (1, 1000000);
  INSERT INTO test SELECT g, g FROM generate_series(100, 2000) g;
  CREATE INDEX test_j ON test(j);
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
}
step s1_1	{ SELECT k, j FROM test WHERE k = 1; }
step s1_2a	{ INSERT INTO test VALUES (1, 2); }
step s1_2b	{ INSERT INTO test VALUES (1, 2) ON CONFLICT DO NOTHING; }
step s1_3	{ SELECT k, j FROM test WHERE k = 1 ORDER BY j; }
step c1		{ COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET enable_seqscan = off;
}
step s2_1	{ DELETE FROM test WHERE j = 1000000; }
step c2		{ COMMIT; }

permutation s1_1 s2_1 c2 s1_2a s1_3 c1
permutation s1_1 s2_1 c2 s1_2b s1_3 c1
