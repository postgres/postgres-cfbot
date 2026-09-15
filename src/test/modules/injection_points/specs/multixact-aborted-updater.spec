# Test multixact expansion with an aborted updater still in ProcArray.
#
# Pause session 1 after recording its abort in pg_xact, before ProcArray cleanup.
# Session 3 can update without waiting, but expanding the multixact must discard
# session 1 to avoid having two updating members.

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
step wake		{
	SELECT FROM injection_points_detach('transaction-abort-after-clog');
	SELECT FROM injection_points_wakeup('transaction-abort-after-clog');
}

permutation
	s1begin
	s1update
	s2lock
	# Pause after recording the abort, before ProcArray cleanup.
	s1abort
	# Expand the multixact without retaining the aborted updater.
	s3update
	wake
