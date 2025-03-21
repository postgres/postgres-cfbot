# index-only-scan test showing wrong results with SPGiST
#
setup
{
	-- by using a low fillfactor and a wide tuple we can get multiple blocks
	-- with just few rows
	CREATE TABLE ios_needs_cleanup_lock (a point NOT NULL, b int not null, pad char(1024) default '')
	WITH (AUTOVACUUM_ENABLED = false, FILLFACTOR = 10);

	INSERT INTO ios_needs_cleanup_lock SELECT point(g.i, g.i), g.i FROM generate_series(1, 10) g(i);

	CREATE INDEX ios_spgist_a ON ios_needs_cleanup_lock USING spgist(a);
}
setup
{
	VACUUM (ANALYZE) ios_needs_cleanup_lock;
}

teardown
{
	DROP TABLE ios_needs_cleanup_lock;
}


session s1

# Force an index-only scan, where possible:
setup {
	SET enable_bitmapscan = false;
	SET enable_indexonlyscan = true;
	SET enable_indexscan = true;
}

step s1_begin { BEGIN; }
step s1_commit { COMMIT; }

step s1_prepare_sorted {
	DECLARE foo NO SCROLL CURSOR FOR SELECT a <-> point '(0,0)' as x FROM ios_needs_cleanup_lock ORDER BY a <-> point '(0,0)';
}

step s1_prepare_unsorted {
	DECLARE foo NO SCROLL CURSOR FOR SELECT a FROM ios_needs_cleanup_lock WHERE box '((-100,-100),(100,100))' @> a;
}

step s1_fetch_1 {
	FETCH FROM foo;
}

step s1_fetch_all {
	FETCH ALL FROM foo;
}


session s2

# Don't delete row 1 so we have a row for the cursor to "rest" on.
step s2_mod
{
  DELETE FROM ios_needs_cleanup_lock WHERE a != point '(1,1)';
}

# Disable truncation, as otherwise we'll just wait for a timeout while trying
# to acquire the lock
step s2_vacuum  { VACUUM (TRUNCATE false) ios_needs_cleanup_lock; }

permutation
  # delete nearly all rows, to make issue visible
  s2_mod
  # create a cursor
  s1_begin
  s1_prepare_sorted

  # fetch one row from the cursor, that ensures the index scan portion is done
  # before the vacuum in the next step
  s1_fetch_1

  # with the bug this vacuum will mark pages as all-visible that the scan in
  # the next step then considers all-visible, despite all rows from those
  # pages having been removed.
  # Because this should block on buffer-level locks, this won't ever be
  # considered "blocked" by isolation tester, and so we only have a single
  # step we can work with concurrently.
  s2_vacuum

  # if this returns any rows, we're busted
  s1_fetch_all

  s1_commit

permutation
  # delete nearly all rows, to make issue visible
  s2_mod
  # create a cursor
  s1_begin
  s1_prepare_unsorted

  # fetch one row from the cursor, that ensures the index scan portion is done
  # before the vacuum in the next step
  s1_fetch_1

  # with the bug this vacuum will mark pages as all-visible that the scan in
  # the next step then considers all-visible, despite all rows from those
  # pages having been removed.
  # Because this should block on buffer-level locks, this won't ever be
  # considered "blocked" by isolation tester, and so we only have a single
  # step we can work with concurrently.
  s2_vacuum

  # if this returns any rows, we're busted
  s1_fetch_all

  s1_commit
