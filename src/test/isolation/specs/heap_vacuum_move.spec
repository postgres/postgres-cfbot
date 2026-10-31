# heap control for flux_vacuum_move: same steps, same
# output.  Ordinary VACUUM must not move a row out from under its writer.
#
# s1 updates the last row in place and keeps the transaction open.  s2
# VACUUMs the table (lower pages are empty after setup's DELETE) and inserts
# enough rows to reuse any slot VACUUM might have freed.  s1 must still see
# exactly its own row, and after it ends every row must be intact.

setup
{
  CREATE TABLE hvmove (id int, v int, pad text) USING heap WITH (autovacuum_enabled = off);
  CREATE INDEX hvmove_id ON hvmove (id);
  INSERT INTO hvmove SELECT g, 0, repeat('x', 200) FROM generate_series(1, 400) g;
  DELETE FROM hvmove WHERE id <= 300;
}
setup		{ VACUUM hvmove; }
setup		{ VACUUM hvmove; }

teardown
{
  DROP TABLE hvmove;
}

session s1
setup		{ SET enable_seqscan = off; SET enable_bitmapscan = off; }
step s1_upd	{ BEGIN; UPDATE hvmove SET v = 7 WHERE id = 400; }
step s1_idx	{ SELECT id, v FROM hvmove WHERE id = 400; }
step s1_bmp	{ SET enable_indexscan = off; SET enable_bitmapscan = on; SELECT id, v FROM hvmove WHERE id = 400; }
step s1_commit	{ COMMIT; }
step s1_rollback { ROLLBACK; }

session s2
step s2_vac	{ VACUUM hvmove; }
step s2_ins	{ INSERT INTO hvmove SELECT g, 100, repeat('n', 200) FROM generate_series(9000, 9800) g; }
step s2_check
{
  SET enable_indexscan = off; SET enable_bitmapscan = off;
  SELECT count(*) AS n, count(DISTINCT id) AS ids, sum(v) AS sumv,
         count(*) FILTER (WHERE id = 400) AS n400,
         max(v) FILTER (WHERE id = 400) AS v400
    FROM hvmove;
}

permutation s1_upd s2_vac s2_ins s1_idx s1_bmp s1_rollback s2_check
permutation s1_upd s2_vac s2_ins s1_idx s1_commit s2_check
