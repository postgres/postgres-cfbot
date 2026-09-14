# FLUX: a speculative insertion that is killed (INSERT ... ON CONFLICT losing
# the race to a concurrent inserter) must delete the TOAST values it stored,
# as heap_abort_speculative does.  Otherwise the loser's values stay in the
# TOAST relation, referenced by nothing, once its transaction commits.
#
# Same control flow as insert-conflict-specconflict's TOAST permutation, on a
# FLUX table whose data column is always stored out of line.

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
  CREATE FUNCTION flux_sct_chunks() RETURNS bigint LANGUAGE plpgsql AS $$
  DECLARE
    n bigint;
  BEGIN
    EXECUTE format('SELECT count(DISTINCT chunk_id) FROM %s',
                   (SELECT reltoastrelid::regclass FROM pg_class
                    WHERE oid = 'flux_sct'::regclass))
    INTO n;
    RETURN n;
  END;$$;
}

teardown
{
  DROP TABLE flux_sct;
  DROP FUNCTION flux_sct_lock(text), flux_sct_val(), flux_sct_chunks();
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
step c_show { SELECT count(*) AS nrows, flux_sct_chunks() AS nvalues,
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
