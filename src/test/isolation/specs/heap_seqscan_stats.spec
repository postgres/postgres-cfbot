# heap control for flux_seqscan_stats.spec, which runs the same steps
# on FLUX; the two expected outputs are identical.
setup
{
  CREATE TABLE ss (k int, v int) USING heap;
  INSERT INTO ss SELECT g, g FROM generate_series(1, 5) g;
}
teardown
{
  DROP TABLE ss;
}

session s1
setup		{ SET enable_indexscan = off; SET enable_bitmapscan = off; }
step s1scan	{ SELECT count(*) FROM ss; }
step s1where	{ SELECT k FROM ss WHERE v > 3 ORDER BY k; }
step s1ff	{ SELECT pg_stat_force_next_flush(); }
step s1stats	{
  SELECT pg_stat_get_numscans('ss'::regclass) AS seq_scan,
         pg_stat_get_tuples_returned('ss'::regclass) AS seq_tup_read;
}

permutation s1stats s1scan s1ff s1stats s1where s1ff s1stats
