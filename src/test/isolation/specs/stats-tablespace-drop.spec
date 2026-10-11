# Test that flushing pending statistics of a backend does not bring back the
# statistics entry of a tablespace that has been dropped in the meantime.

setup { SET allow_in_place_tablespaces = on; }
setup { CREATE TABLESPACE regress_tblspace_drop LOCATION ''; }
setup
{
	CREATE TABLE t_drop (a int) TABLESPACE regress_tblspace_drop;
	INSERT INTO t_drop SELECT generate_series(1, 100);
	CREATE TABLE ts_oid AS
	  SELECT oid FROM pg_tablespace WHERE spcname = 'regress_tblspace_drop';
}

# Session teardowns run before this, so t_drop is already gone.
teardown { DROP TABLESPACE IF EXISTS regress_tblspace_drop; }

session s1
setup
{
	SET debug_parallel_query = off;
	SELECT count(*) FROM t_drop;
	SELECT pg_stat_force_next_flush();
}
step s1_read	{ SELECT count(*) FROM t_drop; }
step s1_temp
{
	SET temp_tablespaces = regress_tblspace_drop;
	SET work_mem = '64kB';
	SELECT count(*) FROM (SELECT * FROM generate_series(1, 10000) g ORDER BY g DESC) s;
}
step s1_flush	{ SELECT pg_stat_force_next_flush(); }

session s2
step s2_drop_table	{ DROP TABLE t_drop; }
step s2_drop_ts		{ DROP TABLESPACE regress_tblspace_drop; }
step s2_check
{
	SELECT EXISTS (SELECT FROM pg_tablespace t WHERE t.oid = o.oid) AS in_catalog,
	       pg_stat_have_stats('tablespace', 0, o.oid::int8) AS have_stats
	FROM ts_oid o;
}
teardown
{
	DROP TABLE IF EXISTS t_drop;
	DROP TABLE ts_oid;
}

# s1 has counted a read when the tablespace goes away, and flushes afterwards
permutation s2_check s1_read s2_drop_table s2_drop_ts s2_check s1_flush s2_check

# the same with a temporary file
permutation s2_check s1_temp s2_drop_table s2_drop_ts s2_check s1_flush s2_check
