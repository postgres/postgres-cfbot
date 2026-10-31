# FLUX: TID range and TABLESAMPLE scans serve the snapshot's version of a row
# updated in place after the scan began.
#
# The scan returns row 1, then blocks on an advisory lock while another
# session UPDATEs (or DELETEs) both rows and commits.  Row 2, read after
# that, must still be the version the scan's snapshot sees, reconstructed
# from UNDO, as heap returns it.  (Based on the review harness
# collection_{flux,heap}_{tid,sample}.)

setup
{
  CREATE TABLE t (id int, v text) USING flux;
  CREATE INDEX ti ON t (id);
  INSERT INTO t VALUES (1, 'aaaa'), (2, 'cccc');
}

teardown
{
  DROP TABLE t;
}

session w
step lock   { SELECT pg_advisory_lock(881); }
step upd    { UPDATE t SET v = CASE id WHEN 1 THEN 'bbbb' ELSE 'dddd' END; }
step del    { DELETE FROM t WHERE id = 2; }
step unlock { SELECT pg_advisory_unlock(881); }

session tid
setup       { SET enable_seqscan = off; SET enable_indexscan = off;
              SET enable_bitmapscan = off; }
step tplan  { EXPLAIN (COSTS OFF) SELECT v FROM t WHERE ctid >= '(0,1)'; }
step tread  { SELECT id, v, CASE WHEN id = 1 THEN pg_advisory_lock(881) END IS NULL AS waited
              FROM t WHERE ctid >= '(0,1)'; }
step tdone  { SELECT pg_advisory_unlock_all(); }

session smp
step splan  { EXPLAIN (COSTS OFF) SELECT v FROM t TABLESAMPLE SYSTEM (100); }
step sread  { SELECT id, v, CASE WHEN id = 1 THEN pg_advisory_lock(881) END IS NULL AS waited
              FROM t TABLESAMPLE SYSTEM (100); }
step sdone  { SELECT pg_advisory_unlock_all(); }

permutation tplan lock tread upd unlock tdone
permutation tplan lock tread del unlock tdone
permutation splan lock sread upd unlock sdone
permutation splan lock sread del unlock sdone
