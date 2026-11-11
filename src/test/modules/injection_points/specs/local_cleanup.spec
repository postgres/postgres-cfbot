# Check that cleanup of a local injection point does not detach another point
# that was attached later using the same name.

setup
{
	CREATE EXTENSION injection_points;
}
teardown
{
	DROP EXTENSION injection_points;
}

# Attach a local injection point and wait in its callback, keeping this
# backend alive while another backend replaces the point.
session s1
setup	{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('injection-points-local-cleanup', 'wait');
}
step wait1	{
	SELECT injection_points_run('injection-points-local-cleanup');
}

# Replace the local point, then verify that the replacement survives cleanup
# of the backend that owned the original point.
session s2
step replace2	{
	SELECT injection_points_detach('injection-points-local-cleanup');
	SELECT injection_points_attach('injection-points-local-cleanup', 'notice');
}
step run2	{
	SELECT injection_points_run('injection-points-local-cleanup');
}
step detach2	{
	SELECT injection_points_detach('injection-points-local-cleanup');
}

# Terminate s1 to run its before_shmem_exit cleanup.  noop3 is needed to ensure
# wait1 has fully completed before starting run2.
session s3
step terminate3	{
	SELECT pg_terminate_backend(pid) FROM pg_stat_activity
	  WHERE wait_event = 'injection-points-local-cleanup';
}
step noop3	{ }

# Keep this permutation last because s1 cannot be reused after termination.
permutation wait1 replace2 terminate3(wait1) noop3 run2 detach2
