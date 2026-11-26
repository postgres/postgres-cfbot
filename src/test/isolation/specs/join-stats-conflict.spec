# Tests for locking conflicts with CREATE STATISTICS on a join.
#
# The role owns fact, the table the statistics object belongs to, and
# needs only SELECT on dim.

setup
{
	CREATE ROLE regress_join_stats_conflict;
	CREATE TABLE dim (id int PRIMARY KEY, label text);
	CREATE TABLE fact (dim_id int);
	ALTER TABLE fact OWNER TO regress_join_stats_conflict;
	GRANT CREATE ON SCHEMA public TO regress_join_stats_conflict;
}

teardown
{
	DROP TABLE fact, dim;
	REVOKE CREATE ON SCHEMA public FROM regress_join_stats_conflict;
	DROP ROLE regress_join_stats_conflict;
}

session s1
step s1_begin		{ BEGIN; }
step s1_lock_exclusive	{ LOCK dim IN ACCESS EXCLUSIVE MODE; }
step s1_lock_share_update	{ LOCK dim IN SHARE UPDATE EXCLUSIVE MODE; }
step s1_vacuum		{ VACUUM dim; }
step s1_commit		{ COMMIT; }

session s2
step s2_grant		{ GRANT SELECT ON dim TO regress_join_stats_conflict; }
step s2_auth		{ SET ROLE regress_join_stats_conflict; }
step s2_begin		{ BEGIN; }
step s2_create
{
	CREATE STATISTICS join_stat (mcv) ON dim.label
		FROM fact JOIN dim ON (fact.dim_id = dim.id);
}
step s2_commit		{ COMMIT; }
step s2_reset		{ RESET ROLE; }

# The role has SELECT on dim, CREATE STATISTICS will block if another session
# holds an exclusive lock on dim, as a query would.
permutation s2_grant s1_begin s1_lock_exclusive s2_auth s2_create s1_commit s2_reset

# CREATE STATISTICS only reads dim, so it does not wait for the lock VACUUM
# and ANALYZE take on it
permutation s2_grant s1_begin s1_lock_share_update s2_auth s2_create s1_commit s2_reset

# nor does VACUUM of dim wait for CREATE STATISTICS
permutation s2_grant s2_auth s2_begin s2_create s1_vacuum s2_commit s2_reset
