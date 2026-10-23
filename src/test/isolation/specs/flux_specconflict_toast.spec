# FLUX: a speculative insertion that is killed (INSERT ... ON CONFLICT losing
# the race to a concurrent inserter) must store its wide value out of line and
# then, when the speculative tuple is cancelled, leave the table holding only
# the winner's row with the winner's value intact.  Regression guard for the
# speculative-insert overflow write path, which used to trap
# Assert(MyFluxXactState != NULL) in FluxGetDmlTimestamp (the per-xact FLUX MVCC
# state was not initialized before FluxStoreOverflowColumn read the DML
# timestamp on the speculative path).
#
# Same control flow as insert-conflict-specconflict's TOAST permutation, on a
# FLUX table whose data column is always stored out of line.  The loser's
# cancelled value becomes a reclaimable overflow orphan (reclaimed lazily at
# VACUUM, not synchronously like heap TOAST), so this test asserts the visible
# end state -- one row, the winner's value -- not the heap-TOAST chunk count.

setup
{
  CREATE FUNCTION flux_sct_lock(text) RETURNS text IMMUTABLE LANGUAGE plpgsql AS $$
  BEGIN
    IF pg_try_advisory_xact_lock(current_setting('spec.session')::int, 1) THEN
      PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 2);
    ELSE
      PERFORM pg_advisory_xact_lock(current_setting('spec.session')::int, 3);
    END IF;
    RETURN $1;
  END;$$;
  CREATE FUNCTION flux_sct_val() RETURNS text LANGUAGE sql AS $$
    SELECT string_agg(encode(sha256(g::text::bytea), 'hex'), '')
    FROM generate_series(1, 133) g $$;
  CREATE TABLE flux_sct (key text, data text) USING flux;
  ALTER TABLE flux_sct ALTER COLUMN data SET STORAGE EXTERNAL;
  CREATE UNIQUE INDEX flux_sct_key ON flux_sct ((flux_sct_lock(key)));
}

teardown
{
  DROP TABLE flux_sct;
  DROP FUNCTION flux_sct_lock(text), flux_sct_val();
}

session controller
step c_locks { SELECT pg_advisory_lock(sess, lock), sess, lock FROM generate_series(1, 2) a(sess), generate_series(1, 3) b(lock); }
step c_unlock_1_1 { SELECT pg_advisory_unlock(1, 1); }
step c_unlock_2_1 { SELECT pg_advisory_unlock(2, 1); }
step c_unlock_1_2 { SELECT pg_advisory_unlock(1, 2); }
step c_unlock_2_2 { SELECT pg_advisory_unlock(2, 2); }
step c_unlock_1_3 { SELECT pg_advisory_unlock(1, 3); }
step c_unlock_2_3 { SELECT pg_advisory_unlock(2, 3); }
# One row; one TOAST value, the winner's.
step c_show { SELECT count(*) AS nrows,
                     bool_and(md5(data) = md5(flux_sct_val())) AS ok
              FROM flux_sct; }

session s1
setup { SET spec.session = 1; }
step s1_insert { INSERT INTO flux_sct VALUES ('k', flux_sct_val()) ON CONFLICT DO NOTHING; }

session s2
setup { SET spec.session = 2; }
step s2_insert { INSERT INTO flux_sct VALUES ('k', flux_sct_val()) ON CONFLICT DO NOTHING; }

permutation
  c_locks
  s1_insert s2_insert
  # both pass the conflict probe and insert speculatively
  c_unlock_1_1 c_unlock_2_1
  c_unlock_1_3 c_unlock_2_3
  # s1 finishes first and commits
  c_unlock_1_2
  # s2 finds the conflict, kills its tuple, does nothing, and commits
  c_unlock_2_2
  c_show
