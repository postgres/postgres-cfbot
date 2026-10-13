# FLUX: an old snapshot reads its version of a row through a version chain
# longer than any fixed bound.
#
# A REPEATABLE READ snapshot is taken, then another session commits 10010
# in-place UPDATEs of one row, one transaction each.  The snapshot must still
# see the row as it was, by walking the whole UNDO version chain back, as
# heap returns its old tuple version.

setup
{
  CREATE TABLE pvs_h (id int, v int) USING heap;
  CREATE TABLE pvs_f (id int, v int) USING flux;
  INSERT INTO pvs_h VALUES (1, 0), (2, 0);
  INSERT INTO pvs_f VALUES (1, 0), (2, 0);
  CREATE PROCEDURE pvs_churn(n int) LANGUAGE plpgsql AS $$
  BEGIN
    FOR i IN 1..n LOOP
      UPDATE pvs_h SET v = i WHERE id = 1;
      UPDATE pvs_f SET v = i WHERE id = 1;
      COMMIT;
    END LOOP;
  END $$;
}

teardown
{
  DROP TABLE pvs_h, pvs_f;
  DROP PROCEDURE pvs_churn(int);
}

session old
setup       { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step snap   { SELECT 'heap' AS am, * FROM pvs_h UNION ALL
              SELECT 'flux', * FROM pvs_f ORDER BY 1, 2; }
step read   { SELECT 'heap' AS am, * FROM pvs_h UNION ALL
              SELECT 'flux', * FROM pvs_f ORDER BY 1, 2; }
step done   { COMMIT; }

session churn
setup       { SET synchronous_commit = off; }
step upd    { CALL pvs_churn(10010); }
step now    { SELECT 'heap' AS am, * FROM pvs_h UNION ALL
              SELECT 'flux', * FROM pvs_f ORDER BY 1, 2; }

permutation snap upd read done now
