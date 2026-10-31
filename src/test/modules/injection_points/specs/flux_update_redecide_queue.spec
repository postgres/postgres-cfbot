# An UPDATE that waits for a writer of its row keeps its place in the queue
# of transactions waiting for that row, as heap_update does by holding the
# tuple lock until it is done.  FLUX decides an UPDATE's index work from the
# image it reads before waiting; when the writer it waited for rolls back,
# the row is another version and the UPDATE decides again
# (flux-update-redecide).  It must still hold the tuple lock then, or a
# DELETE queued behind it deletes the row first.
#
# s1 changes k, s2 sets k to the same value and waits for s1, s3 deletes the
# row and queues behind s2; s1 rolls back.  On heap s2 updates the row and
# s3 then deletes s2's version.  s2 and s3 run in autocommit, so neither
# needs a commit step that could wait behind the other.  The step sync waits
# until s2 is at the injection point (or done) and s3 is either done or
# blocked by s2, whichever happens; wake then lets s2 go on, and s3sel
# reads the table once s3 is done.  Each permutation runs on FLUX (pick_fx)
# and heap (pick_hp), which must return the same rows; heap's s2 does not
# stop at the injection point, so its steps complete right after s1r.

setup
{
  CREATE EXTENSION injection_points;
  CREATE TABLE ur_fx (id int, k int, v int) USING flux;
  CREATE TABLE ur_hp (id int, k int, v int) USING heap;
  CREATE INDEX ON ur_fx (k);
  CREATE INDEX ON ur_hp (k);
  INSERT INTO ur_fx SELECT g, g, 0 FROM generate_series(1, 5) g;
  INSERT INTO ur_hp SELECT g, g, 0 FROM generate_series(1, 5) g;
}
teardown
{
  DROP TABLE IF EXISTS kw, ur_fx, ur_hp;
  DROP EXTENSION injection_points;
}

session s1
step pick_fx	{ ALTER TABLE ur_fx RENAME TO kw; }
step pick_hp	{ ALTER TABLE ur_hp RENAME TO kw; }
step s1b	{ BEGIN; }
step s1k	{ UPDATE kw SET k = 100 WHERE id = 3; }
step s1r	{ ROLLBACK; }

session s2
setup	{
  SELECT injection_points_set_local();
  SELECT injection_points_attach('flux-update-redecide', 'wait');
}
step s2k	{ UPDATE kw SET k = 100, v = 2 WHERE id = 3 RETURNING id, k, v; }
step s2det	{ SELECT injection_points_detach('flux-update-redecide'); }

session s3
step s3d	{ DELETE FROM kw WHERE id = 3 RETURNING id, k, v; }
step s3sel	{ SELECT id, k, v FROM kw ORDER BY id; }

session s4
step sync	{
  DO $$
  DECLARE
    n int := 0;
  BEGIN
    LOOP
      PERFORM pg_stat_clear_snapshot();
      EXIT WHEN EXISTS (
        SELECT FROM pg_stat_activity a2, pg_stat_activity a3
         WHERE a2.application_name LIKE '%/s2'
           AND a3.application_name LIKE '%/s3'
           AND (a2.wait_event = 'flux-update-redecide' OR a2.state = 'idle')
           AND (a3.state = 'idle' OR a2.pid = ANY (pg_blocking_pids(a3.pid))));
      n := n + 1;
      IF n > 30000 THEN
        RAISE EXCEPTION 's2 and s3 did not settle';
      END IF;
      PERFORM pg_sleep(0.01);
    END LOOP;
  END $$;
}
step wake	{
  DO $$
  BEGIN
    IF EXISTS (SELECT FROM pg_stat_activity
                WHERE wait_event = 'flux-update-redecide') THEN
      PERFORM injection_points_wakeup('flux-update-redecide');
    END IF;
  END $$;
}

permutation pick_fx s1b s1k s2k(wake) s3d(s2k) s1r sync wake s3sel s2det
permutation pick_hp s1b s1k s2k(wake) s3d(s2k) s1r sync wake s3sel s2det
