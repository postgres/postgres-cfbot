# REPACK (CONCURRENTLY) of a FLUX table: the changes REPACK replays onto its
# transient table must not reach an output plugin, not even one that asks for
# the changes of table rewrites (see repack_decode.spec, for heap).  Each
# permutation runs on a FLUX table and on its heap twin; heap is the control.

setup
{
  SELECT 'init' FROM pg_create_logical_replication_slot('s', 'test_decoding');
}
setup
{
  CREATE EXTENSION injection_points;
  CREATE TABLE fx (i int PRIMARY KEY, k int, t text) USING flux;
  CREATE TABLE hp (i int PRIMARY KEY, k int, t text) USING heap;
  ALTER TABLE fx ALTER COLUMN t SET STORAGE EXTERNAL;
  ALTER TABLE hp ALTER COLUMN t SET STORAGE EXTERNAL;
  INSERT INTO fx SELECT g, g, repeat(md5(g::text), 100)
	FROM generate_series(1, 3) g;
  INSERT INTO hp SELECT * FROM fx;
}

teardown
{
  DROP TABLE IF EXISTS fx, hp, t;
  DROP EXTENSION injection_points;
  SELECT pg_drop_replication_slot('s');
}

session s1
setup
{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step pick_fx	{ ALTER TABLE fx RENAME TO t; }
step pick_hp	{ ALTER TABLE hp RENAME TO t; }
step rp		{ REPACK (CONCURRENTLY) t; }
# Skip what the setup and the rename did.
step skip	{
  SELECT count(*) > 0 AS skipped FROM pg_logical_slot_get_changes('s', NULL, NULL);
}
step decode	{
  SELECT regexp_replace(data, '''[0-9a-f]+''', '''<md5s>''', 'g') AS data
	FROM pg_logical_slot_get_changes('s', NULL, NULL, 'include-rewrites', '1',
	  'skip-empty-xacts', '1', 'include-xids', '0');
}
teardown
{
  SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
# Replayed by REPACK: an in-place UPDATE of a non-key column to a short
# value, of a key, to a new TOASTed value, an INSERT and a DELETE.
step s2_changes	{
  UPDATE t SET t = 'short' WHERE i = 1;
  UPDATE t SET k = 20 WHERE i = 2;
  UPDATE t SET t = repeat(md5('new'), 100) WHERE i = 3;
  INSERT INTO t VALUES (4, 4, 'four');
  DELETE FROM t WHERE i = 4;
}
step s2_wake	{
  SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation pick_fx skip rp s2_changes s2_wake decode
permutation pick_hp skip rp s2_changes s2_wake decode
