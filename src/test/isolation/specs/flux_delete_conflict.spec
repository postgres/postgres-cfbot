# FLUX concurrent UPDATE/DELETE and DELETE/DELETE behave like heap.
#
# Every permutation runs against a FLUX table; the expected output is heap's
# for the same spec with USING heap (only the table AM differs).  Under READ
# COMMITTED the second writer waits, then re-checks the row (EvalPlanQual);
# under REPEATABLE READ it fails with a serialization error.

setup
{
  CREATE TABLE d (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO d VALUES (1, 10), (2, 20);
}

teardown
{
  DROP TABLE d;
}

session s1
step b1rc   { BEGIN ISOLATION LEVEL READ COMMITTED; }
step b1rr   { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step u1     { UPDATE d SET v = v + 1 WHERE id = 1; }
step d1     { DELETE FROM d WHERE id = 1; }
step c1     { COMMIT; }
step a1     { ROLLBACK; }

session s2
step b2rc   { BEGIN ISOLATION LEVEL READ COMMITTED; }
step b2rr   { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step snap2  { SELECT count(*) FROM d; }
step u2     { UPDATE d SET v = v + 100 WHERE id = 1 RETURNING v; }
step d2     { DELETE FROM d WHERE id = 1 RETURNING v; }
step c2     { COMMIT; }
step sel    { SELECT id, v FROM d ORDER BY id; }

# READ COMMITTED
permutation b1rc b2rc d1 d2 c1 c2 sel
permutation b1rc b2rc d1 d2 a1 c2 sel
permutation b1rc b2rc u1 d2 c1 c2 sel
permutation b1rc b2rc d1 u2 c1 c2 sel
permutation b1rc b2rc d1 u2 a1 c2 sel
# REPEATABLE READ (s2's snapshot taken before s1 writes)
permutation b1rr b2rr snap2 d1 d2 c1 c2 sel
permutation b1rr b2rr snap2 d1 d2 a1 c2 sel
permutation b1rr b2rr snap2 u1 d2 c1 c2 sel
permutation b1rr b2rr snap2 d1 u2 c1 c2 sel
permutation b1rr b2rr snap2 d1 c1 d2 c2 sel
