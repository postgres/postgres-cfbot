# REPACK (CONCURRENTLY) of one table while another table changes.
#
# REPACK's decoding worker decodes the WAL of the whole database but must
# keep only the changes of the table it repacks (and its TOAST table); each
# rmgr's decode routine asks change_useless_for_repack() to drop the rest.
# The repacked table here is heap; the other table is FLUX in one run and
# heap (the control) in the other.  Both must leave the repacked table as it
# was, and the other table's changes in the other table only.

setup
{
  CREATE EXTENSION amcheck;
  CREATE EXTENSION injection_points;
  CREATE TABLE r (id int PRIMARY KEY, k int, v text) USING heap;
  CREATE INDEX r_k ON r (k);
  INSERT INTO r SELECT g, g, 'r' FROM generate_series(1, 20) g;
  -- v is PLAIN and the first page full, so growing v moves the row
  CREATE TABLE ofx (id int PRIMARY KEY, k int, v text) USING flux
	WITH (fillfactor = 100);
  CREATE TABLE ohp (id int PRIMARY KEY, k int, v text) USING heap
	WITH (fillfactor = 100);
  ALTER TABLE ofx ALTER COLUMN v SET STORAGE PLAIN;
  ALTER TABLE ohp ALTER COLUMN v SET STORAGE PLAIN;
  INSERT INTO ofx SELECT g, g * 100, repeat('o', 100)
	FROM generate_series(1, 120) g;
  INSERT INTO ohp SELECT * FROM ofx;
  -- COPY FROM inserts in batches (FLUX: XLOG_FLUX_MULTI_INSERT)
  DO $$ BEGIN
    EXECUTE format('COPY (SELECT g, g, %L FROM generate_series(1030, 1039) g) TO %L',
                   'copied', current_setting('data_directory') || '/repack_other_table.dat');
  END $$;
}

teardown
{
  DROP TABLE IF EXISTS r, ofx, ohp, o;
  DROP EXTENSION amcheck;
  DROP EXTENSION injection_points;
}

session s
setup
{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step rp		{ REPACK (CONCURRENTLY) r; }
step chk	{
  SELECT count(*), sum(id), sum(k), string_agg(DISTINCT v, ',') AS v FROM r;
  SELECT bt_index_check(indexrelid, true) FROM pg_index
	WHERE indrelid = 'r'::regclass;
  SELECT count(*), sum(id), sum(k) FROM o;
}
teardown
{
  SELECT injection_points_detach('repack-concurrently-before-lock');
}

session w
step pick_fx	{ ALTER TABLE ofx RENAME TO o; }
step pick_hp	{ ALTER TABLE ohp RENAME TO o; }
# Keys that also exist in r, so that a change replayed onto r would find a row.
step w_dml	{
  INSERT INTO o VALUES (1001, 1001, 'other');
  UPDATE o SET k = 2222, v = 'other' WHERE id = 2;
  UPDATE o SET id = 1003 WHERE id = 3;
  DELETE FROM o WHERE id = 4;
  INSERT INTO o SELECT g, g, 'other' FROM generate_series(1010, 1019) g;
  INSERT INTO o VALUES (5, 0, 'other') ON CONFLICT (id) DO UPDATE SET k = 5555;
  INSERT INTO o VALUES (1020, 0, 'other') ON CONFLICT (id) DO NOTHING;
  UPDATE o SET v = repeat('x', 2000) WHERE id = 6;
  SELECT (ctid::text::point)[0] > 0 AS moved FROM o WHERE id = 6;
  DO $$ BEGIN
    EXECUTE format('COPY o FROM %L',
                   current_setting('data_directory') || '/repack_other_table.dat');
  END $$;
}
step wake	{
  SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation pick_fx rp w_dml wake chk
permutation pick_hp rp w_dml wake chk
