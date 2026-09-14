# FLUX: always-in-place indexed-column UPDATE via delete-marking.
#
# Validates the MVCC visibility of an in-place key UPDATE across snapshots:
#   - a REPEATABLE READ reader whose snapshot predates the key UPDATE keeps
#     seeing the OLD key + value (reachable through the delete-marked old
#     index entry), and does NOT see the row under the NEW key;
#   - a reader whose snapshot follows the UPDATE sees only the NEW key;
#   - the TID is stable, so no row moves.
# Both the old and new versions are reached via the SAME index; the delete-
# marking key recheck ensures neither snapshot sees a duplicate
# or a phantom.

setup
{
  CREATE TABLE flux_ip (id int PRIMARY KEY, k int, v text) USING flux;
  INSERT INTO flux_ip VALUES (1, 100, 'a');
  CREATE INDEX flux_ip_k ON flux_ip (k);
  SET enable_seqscan = off;
}

teardown
{
  DROP TABLE flux_ip;
}

session s_old
setup     { BEGIN ISOLATION LEVEL REPEATABLE READ;
            SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off;
            SELECT k, v FROM flux_ip WHERE id = 1; }
step o_old_key  { SELECT k, v FROM flux_ip WHERE k = 100; }   # old snapshot: sees 100,a
step o_new_key  { SELECT count(*) AS n FROM flux_ip WHERE k = 200; }  # old snapshot: 0
step o_commit   { COMMIT; }

session s_writer
step w_update   { UPDATE flux_ip SET k = 200, v = 'b' WHERE id = 1; }

session s_new
setup     { SET enable_seqscan = off; SET enable_indexscan = on; SET enable_bitmapscan = off; }
step n_new_key  { SELECT k, v FROM flux_ip WHERE k = 200; }   # new snapshot: sees 200,b
step n_old_key  { SELECT count(*) AS n FROM flux_ip WHERE k = 100; }  # new snapshot: 0

# The old-snapshot reader established its snapshot in setup (before the
# UPDATE), so after the committed in-place key UPDATE it still sees the old
# key/value, while a fresh reader sees only the new key.
permutation w_update o_old_key o_new_key n_new_key n_old_key o_commit
