# INSERT...ON CONFLICT DO SELECT test at SERIALIZABLE
#
# Returning the existing row is a read for SSI purposes, so a concurrent
# transaction writing that row must create a rw-antidependency.  These
# permutations build the classic write-skew cycle: s1 reads a and writes
# b, while s2 reads b and writes a.  One of the two transactions must
# fail with a serialization error.

setup
{
  CREATE TABLE doselect_a (key int PRIMARY KEY, val int);
  CREATE TABLE doselect_b (key int PRIMARY KEY, val int);
  INSERT INTO doselect_a VALUES (1, 0);
}

teardown
{
  DROP TABLE doselect_a, doselect_b;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step select1 { INSERT INTO doselect_a VALUES (1, 99) ON CONFLICT (key) DO SELECT RETURNING val; }
step select1_keyshare { INSERT INTO doselect_a VALUES (1, 99) ON CONFLICT (key) DO SELECT FOR KEY SHARE RETURNING val; }
step select1_where { INSERT INTO doselect_a VALUES (1, 99) ON CONFLICT (key) DO SELECT WHERE doselect_a.val > 100 RETURNING val; }
step insert1 { INSERT INTO doselect_b VALUES (1, 10); }
step c1 { COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
}
step count2 { SELECT count(*) FROM doselect_b; }
step update2 { UPDATE doselect_a SET val = 1 WHERE key = 1; }
step c2 { COMMIT; }

# Write skew with DO SELECT: s2 must fail to commit
permutation select1 count2 update2 insert1 c1 c2

# Write skew with DO SELECT FOR KEY SHARE: the non-key update does not
# conflict with the tuple lock, so update2 proceeds without blocking and
# only the SIREAD lock makes it fail
permutation select1_keyshare count2 update2 insert1 c1 c2

# If the update is already in flight when DO SELECT runs, the arbiter
# probe waits for it to commit, leaving a conflicting row that is not
# visible to the query snapshot.  The row cannot be returned, so DO
# SELECT must fail instead
permutation count2 update2 select1 c2 insert1 c1

# Write skew with a DO SELECT WHERE clause that rejects the existing row:
# the row is not returned, but it decided the outcome of the INSERT and
# was examined by the WHERE clause, so it still counts as a read
permutation select1_where count2 update2 insert1 c1 c2
