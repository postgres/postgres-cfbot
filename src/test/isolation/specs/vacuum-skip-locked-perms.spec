# Test that lock-skip statistics are not recorded for a role that lacks the
# privileges to VACUUM or ANALYZE the table.
#
# A role that cannot vacuum or analyze a table must not be able to change that
# table's statistics, so a skip caused by a lock held by someone else must not
# be counted when the caller would not have been permitted to process the table
# anyway.  The second permutation is the control: the same skips are counted
# for a role that is allowed to process the table.

setup
{
	CREATE ROLE regress_vsl_unpriv;
	CREATE TABLE vsl_tab (a int);
	ALTER TABLE vsl_tab SET (autovacuum_enabled = false);
}

teardown
{
	DROP TABLE vsl_tab;
	DROP ROLE regress_vsl_unpriv;
}

session s1
step lock
{
	BEGIN;
	LOCK TABLE vsl_tab IN ACCESS EXCLUSIVE MODE;
}
step commit
{
	COMMIT;
}
step check_stat { SELECT pg_stat_get_lock_skipped_vacuum_count('vsl_tab'::regclass) AS vac_skips, pg_stat_get_lock_skipped_analyze_count('vsl_tab'::regclass) AS anl_skips; }

session s2
step s2_unpriv	{ SET ROLE regress_vsl_unpriv; }
step s2_reset	{ RESET ROLE; }
step s2_vacuum	{ VACUUM (SKIP_LOCKED) vsl_tab; }
step s2_analyze	{ ANALYZE (SKIP_LOCKED) vsl_tab; }

# The role has no privileges on the table: nothing is counted.
permutation lock s2_unpriv s2_vacuum s2_analyze s2_reset commit check_stat

# Control: the owner (the superuser running the test) is counted.
permutation lock s2_vacuum s2_analyze commit check_stat
