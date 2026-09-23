# Test for write skew under SERIALIZABLE with a TID scan
#
# TID scan has to take a relation level SIREAD lock for a concurrent
# INSERT materializing a tuple at exactly that TID to conflict with it.

setup
{
  create table ta (id int);
  insert into ta values (1), (2);
  create table tb (id int);
  insert into tb values (1), (2);
}

teardown
{
  drop table ta;
  drop table tb;
}

session s1
setup
{
  begin isolation level serializable;
  set enable_seqscan = off;
}
step r1	{ select count(*) from tb where ctid = '(0,3)'; }
step w1	{ insert into ta values (100); }
step c1	{ commit; }

session s2
setup
{
  begin isolation level serializable;
  set enable_seqscan = off;
}
step r2	{ select count(*) from ta where ctid = '(0,3)'; }
step w2	{ insert into tb values (200); }
step c2	{ commit; }

permutation r1 r2 w1 w2 c1 c2
