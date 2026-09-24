# REPACK (CONCURRENTLY)
#
# Test columns whose values are "missing" from the existing tuples, because
# ALTER TABLE ... ADD COLUMN did not have to rewrite the table.
setup
{
	CREATE EXTENSION IF NOT EXISTS injection_points;

	CREATE TABLE repack_test(i int PRIMARY KEY, j int);
	INSERT INTO repack_test(i, j) VALUES (1, 1), (2, 2), (3, 3);

	-- A constant default does not rewrite the table, so the rows above keep
	-- their shorter tuples and the value of "c" is only stored in
	-- pg_attribute.attmissingval.
	ALTER TABLE repack_test ADD COLUMN c text NOT NULL DEFAULT 'xyz';

	-- With "c" in the replica identity, a missing value is also used as the key
	-- to find the target tuple, not only to form the new row version.  Use a
	-- pass-by-reference type, because a NULL key of that kind can crash the
	-- comparison function.
	CREATE UNIQUE INDEX repack_test_ident_idx ON repack_test (i, c);
	ALTER TABLE repack_test REPLICA IDENTITY USING INDEX repack_test_ident_idx;

	CREATE FUNCTION repack_return_old() RETURNS trigger
	LANGUAGE plpgsql AS $$
	BEGIN
		RETURN OLD;
	END;
	$$;

	-- By returning OLD, the trigger makes the new row version reuse the
	-- shorter tuple, which is then what logical decoding sees.
	CREATE TRIGGER return_old BEFORE UPDATE ON repack_test
	FOR EACH ROW EXECUTE FUNCTION repack_return_old();
}

teardown
{
	DROP TABLE repack_test;
	DROP FUNCTION repack_return_old();
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}

# Perform the initial load and wait for s2 to change the data.
step s1_wait_before_lock
{
	REPACK (CONCURRENTLY) repack_test;
}

# The missing values must have survived the concurrent changes.
step s1_check
{
	SELECT i, j, c FROM repack_test ORDER BY i;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2

# Update two of the three rows, without changing the data.
step s2_update
{
	UPDATE repack_test SET j = j WHERE i IN (1, 2);
}
step s2_wakeup_before_lock
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation
	s1_wait_before_lock
	s2_update
	s2_wakeup_before_lock
	s1_check
