# Test for race conditions between VACUUM (SKIP_LOCKED) stats reporting
# and concurrent DROP TABLE.
#
# When VACUUM (SKIP_LOCKED) cannot acquire a lock, it reports skipped
# statistics via pgstat_report_skipped_vacuum_analyze().  An injection
# point after the syscache lookup but before the stats update allows us
# to verify that a concurrent DROP does not leave orphaned stats entries.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE test_skip (id int);
	INSERT INTO test_skip VALUES (1);
	ANALYZE test_skip;
	SELECT pg_stat_force_next_flush();
	CREATE TABLE saved_oid (oid_val oid);
	INSERT INTO saved_oid SELECT oid FROM pg_class WHERE relname = 'test_skip';
}

teardown
{
	DROP TABLE IF EXISTS test_skip;
	DROP TABLE IF EXISTS saved_oid;
	DROP EXTENSION injection_points;
}

# s1: holds the lock so VACUUM skips the table
session s1
step lock
{
	BEGIN;
	LOCK TABLE test_skip IN ACCESS EXCLUSIVE MODE;
}
step unlock	{ COMMIT; }

# s2: runs VACUUM (SKIP_LOCKED), blocks at injection point after skip
session s2
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('skipped-vacuum-analyze-before-entry-lock', 'wait');
}
step vacuum	{ VACUUM (SKIP_LOCKED) test_skip; }
step noop {}
step detach	{ SELECT injection_points_detach('skipped-vacuum-analyze-before-entry-lock'); }

# s3: drops table or wakes up the vacuumer
session s3
step drop_table	{ DROP TABLE test_skip; }
step rollback_drop
{
	BEGIN;
	DROP TABLE test_skip;
	ROLLBACK;
}
step wakeup	{ SELECT injection_points_wakeup('skipped-vacuum-analyze-before-entry-lock'); }
step check_stats
{
	SELECT pg_stat_force_next_flush();
	SELECT pg_stat_get_lock_skipped_vacuum_count(oid_val) AS skip_count
	FROM saved_oid;
}

# Table dropped while vacuumer is blocked: no orphaned stats entry.
permutation lock vacuum(wakeup) unlock drop_table wakeup noop check_stats detach

# DROP rolled back while vacuumer is blocked: skip is still recorded.
permutation lock vacuum(wakeup) unlock rollback_drop wakeup noop check_stats detach
