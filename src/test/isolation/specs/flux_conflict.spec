# FLUX concurrent write-conflict isolation tests.
#
# Validates that write-conflict detection (which transaction is concurrently
# modifying a tuple) is correct when derived from the on-page header:
#   - two writers on the SAME row serialize (blocker waits for committer),
#   - two writers on DIFFERENT rows of the SAME page do NOT falsely conflict,
#   - concurrent delete vs update interact like heap.
#
# All rows are packed onto one page (fillfactor 100, tiny rows) so the
# "different rows, same page" case genuinely shares a page.

setup
{
  CREATE TABLE flux_cc (id int PRIMARY KEY, v int) USING flux WITH (fillfactor = 100);
  INSERT INTO flux_cc SELECT g, 0 FROM generate_series(1, 20) g;
}

teardown
{
  DROP TABLE flux_cc;
}

session s1
setup		{ BEGIN; }
step s1_upd1	{ UPDATE flux_cc SET v = 1 WHERE id = 1; }
step s1_del1	{ DELETE FROM flux_cc WHERE id = 1; }
step s1_commit	{ COMMIT; }
step s1_abort	{ ROLLBACK; }

session s2
setup		{ BEGIN; }
step s2_upd1	{ UPDATE flux_cc SET v = 2 WHERE id = 1; }
step s2_upd2	{ UPDATE flux_cc SET v = 2 WHERE id = 2; }
step s2_del1	{ DELETE FROM flux_cc WHERE id = 1; }
step s2_commit	{ COMMIT; }

# Same row: s2's update must block until s1 commits, then apply on the new row.
permutation s1_upd1 s2_upd1 s1_commit s2_commit

# Different rows, SAME page: s2 updating id=2 must NOT block on s1's id=1 update.
permutation s1_upd1 s2_upd2 s1_commit s2_commit

# Concurrent delete (s1) vs update (s2) of the same row: s2 blocks, then finds
# the row deleted after s1 commits.
permutation s1_del1 s2_upd1 s1_commit s2_commit

# Delete then abort: s2's update blocks, then proceeds after s1 rolls back.
permutation s1_del1 s2_upd1 s1_abort s2_commit

# Update vs update on the SAME row: s2 blocks behind s1's in-progress update.
permutation s1_upd1 s2_upd1 s1_abort s2_commit

# Update (s1) vs delete (s2) of the same row: s2's delete blocks behind s1's update.
permutation s1_upd1 s2_del1 s1_commit s2_commit
