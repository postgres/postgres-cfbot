# Test concurrent behavior during an updater's abort window (between the
# pg_xact write and ProcArray cleanup) and after it closes.
#
# Session 1 pauses after recording its abort in pg_xact, before ProcArray
# cleanup.  During the window the updater still reports running, so a
# conflicting NOWAIT lock request fails, and UPDATE and DELETE wait out
# the window.  Once the abort finishes, these operations succeed.  A new
# UPDATE must not retain the aborted updater when expanding the multixact.

setup
{
	CREATE EXTENSION injection_points;

	CREATE TABLE mxact_abort (id int PRIMARY KEY, filler text);
	INSERT INTO mxact_abort VALUES (1, 'initial');
}

teardown
{
	DROP TABLE mxact_abort;
	DROP EXTENSION injection_points;
}

session s1
setup	{
	SELECT FROM injection_points_set_local();
	SELECT FROM injection_points_attach('transaction-abort-after-clog', 'wait');
}
step s1begin	{ BEGIN; }
step s1update	{ UPDATE mxact_abort SET filler = 's1' WHERE id = 1; }
step s1abort	{ ROLLBACK; }

session s2
# The compatible key-share lock makes xmax a multixact with session 1 as updater.
step s2lock		{ SELECT * FROM mxact_abort WHERE id = 1 FOR KEY SHARE; }

session s3
step s3nowait	{ SELECT * FROM mxact_abort WHERE id = 1 FOR NO KEY UPDATE NOWAIT; }
# Omit variable XIDs from the error message.
step s3update	{
	DO $$
	BEGIN
		UPDATE mxact_abort SET filler = 's3' WHERE id = 1;
		RAISE NOTICE 'session 3 update succeeded';
	EXCEPTION WHEN OTHERS THEN
		RAISE NOTICE 'session 3 update failed: %', split_part(SQLERRM, ':', 1);
	END
	$$;
}
step s3delete	{ DELETE FROM mxact_abort WHERE id = 1; }
step s3keyupdate	{ UPDATE mxact_abort SET id = 2 WHERE id = 1; }
step s3check		{ SELECT * FROM mxact_abort ORDER BY id; }

session s4
step wake		{
	SELECT FROM injection_points_detach('transaction-abort-after-clog');
	SELECT FROM injection_points_wakeup('transaction-abort-after-clog');
}

# During the window the updater, though already aborted in pg_xact, still
# reports running: the conflicting NOWAIT request fails.
permutation
	s1begin
	s1update
	s2lock
	# Pause after recording the abort, before ProcArray cleanup.
	s1abort
	s3nowait
	wake

# An ordinary UPDATE waits out the window instead of proceeding; when the
# abort completes, it succeeds; the updater is no longer running when
# multixact expansion examines its members.
# Report the abort after the conflicting operation in these permutations
# to keep the completion output stable.
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3update)
	s3update
	wake

# DELETE must wait rather than report TM_Updated and fail the executor's
# traversed assertion when it re-locks the original tuple.
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3delete)
	s3delete
	wake
	s3check

# A key-changing UPDATE must also wait before deciding which members of
# the multixact survive.  The new key must retain the original row's value.
permutation
	s1begin
	s1update
	s2lock
	s1abort(s3keyupdate)
	s3keyupdate
	wake
	s3check
