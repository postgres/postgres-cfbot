# s1 reads b and deletes a; s2 reads a and deletes or updates b.
# Both reads return the original row, which is impossible in either serial
# order, so at least one transaction must fail.  Separate tables avoid
# index-page conflicts masking the heap race.  The UPDATE sets present to false,
# avoiding a new index entry and its conflict check.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE ios_a (id int PRIMARY KEY) WITH (autovacuum_enabled = false);
	CREATE TABLE ios_b (id int, present bool DEFAULT true)
		WITH (autovacuum_enabled = false);
	CREATE INDEX ios_b_idx ON ios_b (id) WHERE present;
	INSERT INTO ios_a VALUES (1);
	INSERT INTO ios_b VALUES (1);
}
setup { VACUUM (FREEZE, ANALYZE) ios_a; }
setup { VACUUM (FREEZE, ANALYZE) ios_b; }
teardown
{
	DROP TABLE ios_a, ios_b;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
}
step begin1 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step pause_reader
{
	SELECT injection_points_attach('index-only-scan-before-predicate-lock', 'wait');
}
step read1
{
	EXPLAIN (COSTS OFF) SELECT id FROM ios_b WHERE present;
	SELECT id FROM ios_b WHERE present;
}
step delete1 { DELETE FROM ios_a; }
step commit1 { COMMIT; }

session s2
setup
{
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SELECT injection_points_set_local();
}
step begin2 { BEGIN ISOLATION LEVEL SERIALIZABLE; }
step read2 { SELECT id FROM ios_a; }
step pause_delete
{
	SELECT injection_points_attach('heap-delete-before-write', 'wait');
}
step pause_update
{
	SELECT injection_points_attach('heap-update-before-write', 'wait');
}
step delete2 { DELETE FROM ios_b; }
step update2 { UPDATE ios_b SET present = false; }
step commit2 { COMMIT; }

session control
step wake_delete
{
	SELECT injection_points_detach('heap-delete-before-write');
	SELECT injection_points_wakeup('heap-delete-before-write');
}
step wake_update
{
	SELECT injection_points_detach('heap-update-before-write');
	SELECT injection_points_wakeup('heap-update-before-write');
}
step wake_reader
{
	SELECT injection_points_detach('index-only-scan-before-predicate-lock');
	SELECT injection_points_wakeup('index-only-scan-before-predicate-lock');
}
step wake_done { }

# s1 reads b after s2's first conflict check.  After clearing the VM bit,
# s2 must check again, find s1's predicate lock, and abort.
permutation begin1 begin2 read2 pause_delete delete2 read1 delete1 commit1 wake_delete(delete2) wake_done commit2
permutation begin1 begin2 read2 pause_update update2 read1 delete1 commit1 wake_update(update2) wake_done commit2

# s2 changes b after s1's first VM check but before s1's predicate lock.
# Rechecking the VM must make s1 fetch the heap tuple and detect s2's write,
# so that s1 cannot also commit its delete.
permutation begin1 begin2 read2 pause_reader read1 delete2 commit2 wake_reader(read1) wake_done delete1 commit1
permutation begin1 begin2 read2 pause_reader read1 update2 commit2 wake_reader(read1) wake_done delete1 commit1
