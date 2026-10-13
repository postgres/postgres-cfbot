# FLUX: a scan's row must not change under its reader.
#
# FLUX updates rows in place.  A scan slot that kept pointing into the shared
# page saw whatever was written there later, so a row a query had already
# fetched could turn into a newer version its snapshot must not see.  The
# reader fetches the row, then blocks on an advisory lock while a same-size
# UPDATE rewrites the row and commits; it must still return the value its
# snapshot saw, as heap does.  Whether v is deformed before or after the
# lock wait does not matter: the row was fetched before the UPDATE, and
# either way the answer is the snapshot's 'aaaa'.  One session per scan path
# (plans shown), each against FLUX (ft) and heap (ht).

setup
{
  CREATE TABLE ft (id int, v text) USING flux;
  CREATE INDEX ft_id ON ft (id);
  INSERT INTO ft VALUES (1, 'aaaa');
  CREATE TABLE ht (id int, v text) USING heap;
  CREATE INDEX ht_id ON ht (id);
  INSERT INTO ht VALUES (1, 'aaaa');
}

teardown
{
  DROP TABLE ft;
  DROP TABLE ht;
}

session w
step w_lock   { SELECT pg_advisory_lock(1); }
step w_upd    { UPDATE ft SET v = 'bbbb' WHERE id = 1;
                UPDATE ht SET v = 'bbbb' WHERE id = 1; }
step w_unlock { SELECT pg_advisory_unlock(1); }

session seq
setup         { SET enable_indexscan = off; SET enable_bitmapscan = off;
                SET enable_tidscan = off; }
step seq_plan { EXPLAIN (COSTS OFF) SELECT v FROM ft; }
step seq_f    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ft; }
step seq_h    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ht; }
step seq_unlock { SELECT pg_advisory_unlock(1); }

session bmp
setup         { SET enable_seqscan = off; SET enable_indexscan = off;
                SET enable_tidscan = off; }
step bmp_plan { EXPLAIN (COSTS OFF) SELECT v FROM ft WHERE id = 1; }
step bmp_f    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ft WHERE id = 1; }
step bmp_h    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ht WHERE id = 1; }
step bmp_unlock { SELECT pg_advisory_unlock(1); }

session tid
setup         { SET enable_seqscan = off; SET enable_indexscan = off;
                SET enable_bitmapscan = off; }
step tid_plan { EXPLAIN (COSTS OFF) SELECT v FROM ft WHERE ctid >= '(0,1)'; }
step tid_f    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ft
                WHERE ctid >= '(0,1)'; }
step tid_h    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ht
                WHERE ctid >= '(0,1)'; }
step tid_unlock { SELECT pg_advisory_unlock(1); }

session idx
setup         { SET enable_seqscan = off; SET enable_bitmapscan = off;
                SET enable_tidscan = off; }
step idx_plan { EXPLAIN (COSTS OFF) SELECT v FROM ft WHERE id = 1; }
step idx_f    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ft WHERE id = 1; }
step idx_h    { SELECT v, pg_advisory_lock(1) IS NOT NULL AS waited FROM ht WHERE id = 1; }
step idx_unlock { SELECT pg_advisory_unlock(1); }

permutation seq_plan w_lock seq_f w_upd w_unlock seq_unlock
permutation w_lock seq_h w_upd w_unlock seq_unlock
permutation bmp_plan w_lock bmp_f w_upd w_unlock bmp_unlock
permutation w_lock bmp_h w_upd w_unlock bmp_unlock
permutation tid_plan w_lock tid_f w_upd w_unlock tid_unlock
permutation w_lock tid_h w_upd w_unlock tid_unlock
permutation idx_plan w_lock idx_f w_upd w_unlock idx_unlock
permutation w_lock idx_h w_upd w_unlock idx_unlock
