# Bulk INSERT must check relation predicate locks, not promoted page locks.
setup
{
  CREATE TABLE a (id int) USING heap;
  CREATE TABLE b (id int) USING heap;
}
teardown { DROP TABLE a, b; }
session s1
step b1 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r1 { SELECT count(*) FROM a; }
step w1 { COPY b FROM PROGRAM 'printf "1\n"'; }
step c1 { COMMIT; }
session s2
step b2 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step r2 { SELECT count(*) FROM b; }
step w2 { COPY a FROM PROGRAM 'printf "2\n"'; }
step c2 { COMMIT; }
permutation b1 b2 r1 r2 w1 c1 w2 c2
