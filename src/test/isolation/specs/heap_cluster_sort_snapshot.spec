# CLUSTER and VACUUM FULL are MVCC-safe (see "Caveats" in mvcc.sgml): a
# snapshot taken before the rewrite reads exactly the rows it read before,
# whichever way the rewrite reads the table: a sort (cs_e, an expression
# index), an index scan (cs_g: GiST never sorts), the planner's choice (cs_b)
# or a sequential scan (VACUUM FULL).  s1 takes its snapshot on another
# table, so it holds no lock on cs and the rewrite runs at once.  Before the
# rewrite s2 deletes half the rows, updates every row once or twice, or
# inserts one; s1 must then still read the 20 rows it saw, with their old
# values, by a sequential scan and through the rebuilt index.
# HEAP twin; heap_/flux_cluster_sort_snapshot have identical expected output.
setup
{
	CREATE TABLE cs (k int, v int, p point) USING heap;
	INSERT INTO cs SELECT g, g, point(g, g) FROM generate_series(1, 20) g;
	CREATE INDEX cs_e ON cs ((-k));
	CREATE INDEX cs_b ON cs (k);
	CREATE INDEX cs_g ON cs USING gist (p);
	CREATE TABLE cs_other (x int);
}

teardown
{
	DROP TABLE cs, cs_other;
}

session s1
step s1b	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM cs_other; }
step s1r	{ SELECT count(*), min(k), max(k), sum(v) FROM cs; }
step s1ri	{
	SET LOCAL enable_seqscan = off;
	SET LOCAL enable_bitmapscan = off;
	SELECT count(*), min(k), max(k), sum(v) FROM cs WHERE k > 0;
}
step s1c	{ COMMIT; }

session s2
step s2d	{ DELETE FROM cs WHERE k > 10; }
step s2u	{ UPDATE cs SET v = v + 100; }
step s2i	{ INSERT INTO cs VALUES (21, 21, point(21, 21)); }
step s2cle	{ CLUSTER cs USING cs_e; }
step s2clb	{ CLUSTER cs USING cs_b; }
step s2clg	{ CLUSTER cs USING cs_g; }
step s2vf	{ VACUUM FULL cs; }
step s2vff	{ VACUUM (FULL, FREEZE) cs; }
step s2v	{ VACUUM cs; }
step s2flush	{ SELECT pg_stat_force_next_flush(); }
step s2dead	{ SELECT n_dead_tup FROM pg_stat_user_tables WHERE relname = 'cs'; }
step s2chk	{ SELECT count(*), min(k), max(k), sum(v) FROM cs; }

# Deleted rows, each way of reading the table.
permutation s1b s2d s2cle s1r s1ri s1c s2chk
permutation s1b s2d s2clb s1r s1ri s1c s2chk
permutation s1b s2d s2clg s1r s1ri s1c s2chk
permutation s1b s2d s2vf s1r s1ri s1c s2chk
# Rows updated (in place, for FLUX) once or twice, then some deleted.
permutation s1b s2u s2cle s1r s1ri s1c s2chk
permutation s1b s2u s2u s2clg s1r s1ri s1c s2chk
permutation s1b s2u s2d s2vf s1r s1ri s1c s2chk
# A row s1 must not see, and freezing; VACUUM afterwards.  While s1 holds
# its snapshot VACUUM must keep the old versions.  After s1 commits there is
# no count to check: any other snapshot in the database, such as an
# autovacuum ANALYZE of a catalog, can still keep them.
permutation s1b s2i s2u s2vff s1r s1ri s2flush s2v s2dead s1c s2v s2chk
# No rewrite: the control.
permutation s1b s2d s2u s1r s1c s2chk
