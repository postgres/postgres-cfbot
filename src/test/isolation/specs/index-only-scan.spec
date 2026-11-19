# index-only scan test
#
# This test tries to expose problems with the interaction between index-only
# scans and SSI.
#
# Any overlap between the transactions must cause a serialization failure.

# One row per heap page (wide rows, low fillfactor), so that the scan reads a
# separate visibility map bit for each row
setup
{
  CREATE TABLE ios (id int PRIMARY KEY, pad char(1024) DEFAULT '')
    WITH (fillfactor = 10);
  INSERT INTO ios SELECT generate_series(1, 100);
}
setup { VACUUM FREEZE ANALYZE ios; }

teardown
{
  DROP TABLE ios;
}

session s1
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_bitmapscan = off;
}
# An amgetbatch scan reads ahead in the visibility map, so after returning ten
# rows it has already read the bit for the eleventh row's page
step s1_cursor_move10
{
  EXPLAIN (COSTS OFF) DECLARE c CURSOR FOR SELECT id FROM ios ORDER BY id;
  DECLARE c CURSOR FOR SELECT id FROM ios ORDER BY id;
  MOVE FORWARD 10 FROM c;
}
step s1_fetch_next_row { FETCH 1 FROM c; }
step s1_delete_row1 { DELETE FROM ios WHERE id = 1; }
step s1_commit { COMMIT; }

session s2
setup
{
  BEGIN ISOLATION LEVEL SERIALIZABLE;
  SET LOCAL enable_seqscan = off;
  SET LOCAL enable_bitmapscan = off;
}
step s2_select_row1
{
  EXPLAIN (COSTS OFF) SELECT id FROM ios WHERE id = 1;
  SELECT id FROM ios WHERE id = 1;
}
step s2_delete_row11 { DELETE FROM ios WHERE id = 11; }
step s2_commit { COMMIT; }

# session 2 deletes the eleventh row and commits while session 1's cursor has
# returned ten rows; session 1 then still returns the eleventh row
permutation s1_cursor_move10
    s2_select_row1
    s2_delete_row11             # read ahead by session 1, but not returned yet
    s2_commit
    s1_fetch_next_row           # still returns row 11
    s1_delete_row1              # must fail
    s1_commit

# session 2 reads the first row after session 1 has deleted it, so session 2's
# index-only scan has to fetch the row from the heap
permutation s1_cursor_move10
    s1_delete_row1
    s2_select_row1              # heap fetch, sees session 1's uncommitted delete
    s2_delete_row11
    s1_commit
    s2_commit                   # must fail

# session 1's cursor reads ahead past the eleventh row after session 2 has
# deleted it, so the cursor's index-only scan has to fetch the row from the heap
permutation s2_select_row1
    s2_delete_row11
    s1_cursor_move10            # reads ahead past row 11, whose page is no longer all-visible
    s1_fetch_next_row           # heap fetch, sees session 2's uncommitted delete
    s1_delete_row1
    s2_commit
    s1_commit                   # must fail
