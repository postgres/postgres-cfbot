# FLUX: an UPDATE that meets a row whose last writer is between PRE_COMMIT
# and its CLOG update must wait for it and then re-evaluate on its value.
#
# At PRE_COMMIT FLUX finalizes the writer's sLog ops and clears the on-page
# UNCOMMITTED hint, but the transaction is not committed in CLOG until
# RecordTransactionCommit.  The commit-before-clog-update injection point
# holds s1 exactly in that window.  The row's writer is still on the page
# (t_xmin of the in-place image), so s2 must wait on it rather than overwrite
# the value it cannot see yet; after s1 completes, s2 re-evaluates via
# EvalPlanQual and the row ends at 2 -- heap's result.  Before the writer was
# read from the header, nothing on the page or in the sLog named s1 in this
# window except the transaction-duration tuple lock FLUX used to hold.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE fcw (id int PRIMARY KEY, v int) USING flux;
	INSERT INTO fcw VALUES (1, 0);
}

teardown
{
	DROP TABLE fcw;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SET synchronous_commit = on;
	SELECT injection_points_set_local();
	SELECT injection_points_attach('commit-before-clog-update', 'wait');
}
step s1u	{ UPDATE fcw SET v = v + 1 WHERE id = 1; }
teardown
{
	SELECT injection_points_detach('commit-before-clog-update');
}

session s2
step s2u	{ UPDATE fcw SET v = v + 1 WHERE id = 1; }
step s2r	{ SELECT v FROM fcw WHERE id = 1; }

session s3
step s3w	{ SELECT injection_points_wakeup('commit-before-clog-update'); }

# s1 (autocommit) stops after writing its commit record, before CLOG.  s2's
# UPDATE must block until s3 lets s1 finish, then apply on top of s1's value.
permutation s1u s2u(s1u) s3w s2r
