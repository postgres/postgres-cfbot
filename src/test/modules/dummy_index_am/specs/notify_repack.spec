# Tests for amnotifydelete with REPACK (CONCURRENTLY).
#
# REPACK copies the table and builds new indexes, then replays the changes
# made meanwhile on the copy.  A replayed delete or update must reach the new
# index as the original reached the old one.  s2 holds a lock on the table
# without an XID, so REPACK waits for it before its last replay; s2's changes
# are then replayed under that lock.
#
# Each session loads the library quietly first: its string reloption
# validator prints notices when it is loaded.  Parallel query is off, as a
# parallel worker would load the library again and print them.

setup
{
	CREATE EXTENSION dummy_index_am;
	CREATE TABLE repack_tab (k int4 PRIMARY KEY, v int4);
	CREATE INDEX repack_idx ON repack_tab USING dummy_index_am (v);
	INSERT INTO repack_tab VALUES (1, 10), (2, 20), (3, 30);
}

teardown
{
	DROP TABLE repack_tab;
	DROP EXTENSION dummy_index_am;
}

session s1
setup
{
	SET client_min_messages = warning;
	LOAD 'dummy_index_am';
	RESET client_min_messages;
	SET debug_parallel_query = off;
}
step s1_repack	{ REPACK (CONCURRENTLY) repack_tab; }
step s1_show	{ SELECT * FROM repack_tab ORDER BY k; }

session s2
setup
{
	SET client_min_messages = warning;
	LOAD 'dummy_index_am';
	RESET client_min_messages;
	SET debug_parallel_query = off;
}
step s2_begin	{ BEGIN; SELECT count(*) FROM repack_tab; }
step s2_delete	{ DELETE FROM repack_tab WHERE k = 1; }
step s2_update	{ UPDATE repack_tab SET v = 21 WHERE k = 2; }
step s2_insert	{ INSERT INTO repack_tab VALUES (4, 40); }
step s2_commit	{ COMMIT; }

# A delete and a non-HOT update are reported to the new index too
permutation s2_begin s1_repack s2_delete s2_update s2_commit s1_show

# An insert is not
permutation s2_begin s1_repack s2_insert s2_commit s1_show
