# FLUX lost-update prevention under READ COMMITTED (EvalPlanQual).
#
# Two concurrent transactions each do  UPDATE ... SET v = v + 1  on the SAME
# row.  Under READ COMMITTED, PostgreSQL semantics require BOTH increments to
# land: the second updater must block on the first, and after the first commits
# it re-reads the latest committed version (EvalPlanQual) and re-applies its SET
# expression on top -- so v goes 0 -> 1 -> 2, never 0 -> 1 (a lost update).
#
# FLUX is a stable-TID, in-place AM with no persistent per-tuple xmax; before
# the serialization fix it returned TM_Ok against the stale snapshot value and
# silently lost the second increment.  This spec pins the correct behaviour:
# s2's UPDATE waits for s1, and the final value is 2.

setup
{
  CREATE TABLE flux_lu (id int PRIMARY KEY, v bigint) USING flux;
  INSERT INTO flux_lu VALUES (1, 0);
}

teardown
{
  DROP TABLE flux_lu;
}

session s1
setup     { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s1_upd    { UPDATE flux_lu SET v = v + 1 WHERE id = 1; }
step s1_commit { COMMIT; }

session s2
setup     { BEGIN ISOLATION LEVEL READ COMMITTED; }
step s2_upd    { UPDATE flux_lu SET v = v + 1 WHERE id = 1; }
step s2_commit { COMMIT; }

session s3
step s3_read   { SELECT v FROM flux_lu WHERE id = 1; }   # must be 2, not 1

# s1 updates (0->1), s2's identical UPDATE blocks on s1's row lock, s1 commits,
# s2 wakes -> EvalPlanQual re-reads v=1 -> re-applies +1 -> v=2, then commits.
permutation s1_upd s2_upd s1_commit s2_commit s3_read
