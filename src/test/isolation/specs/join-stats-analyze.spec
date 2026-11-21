# ANALYZE of join statistics while the joined table is locked.
#
# ANALYZE of fact holds ShareUpdateExclusiveLock on fact and then needs
# AccessShareLock on dim to sample the join.  ALTER TABLE dim ALTER COLUMN
# TYPE holds AccessExclusiveLock on dim and then needs ShareUpdateExclusiveLock
# on fact to re-create the statistics.  ANALYZE must not wait for dim, or the
# two deadlock.

setup
{
	CREATE TABLE dim (id int PRIMARY KEY, label text);
	CREATE TABLE fact (dim_id int) WITH (autovacuum_enabled = false);
	ALTER TABLE dim SET (autovacuum_enabled = false);
	INSERT INTO dim SELECT g, 'label ' || g % 3 FROM generate_series(1, 100) g;
	INSERT INTO fact SELECT 1 + g % 100 FROM generate_series(1, 1000) g;
	CREATE STATISTICS join_stat (mcv) ON dim.label
		FROM fact JOIN dim ON (fact.dim_id = dim.id);
}

teardown
{
	DROP TABLE fact, dim;
}

# If ANALYZE waited for dim, this session would detect the deadlock, as in
# the reported case
session s1
setup		{ SET deadlock_timeout = '10ms'; }
step lock_dim
{
	BEGIN;
	LOCK dim IN ACCESS EXCLUSIVE MODE;
}
step alter_dim	{ ALTER TABLE dim ALTER COLUMN label TYPE varchar(20); }
step commit		{ COMMIT; }

session s2
setup			{ SET deadlock_timeout = '10s'; }
step analyze_fact	{ ANALYZE fact; }

permutation lock_dim analyze_fact alter_dim commit
