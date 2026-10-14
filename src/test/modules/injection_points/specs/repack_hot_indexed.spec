# REPACK (CONCURRENTLY) replays changes made while it copied the table.  When
# a replayed update changes the identity key and the table has another index,
# the replay can take the HOT-indexed path, leaving a stale entry for the old
# key.  A later replayed change for a row that once had that key must not be
# matched to the wrong row through the stale entry.
setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE repack_hi(i int PRIMARY KEY, j int, pad text)
		WITH (fillfactor = 50);
	CREATE INDEX repack_hi_j ON repack_hi (j);
	INSERT INTO repack_hi SELECT g, g, 'x' FROM generate_series(1, 20) g;
}

teardown
{
	DROP TABLE repack_hi;
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('repack-concurrently-before-lock', 'wait');
}
step repack
{
	REPACK (CONCURRENTLY) repack_hi USING INDEX repack_hi_pkey;
}
step check
{
	SELECT i, j FROM repack_hi WHERE i IN (1, 2, 100, 101) ORDER BY i;
	SELECT count(*) AS rows FROM repack_hi;
}
teardown
{
	SELECT injection_points_detach('repack-concurrently-before-lock');
}

session s2
# Move key 1 away (to 100), then reuse key 1 for a new row and change it.
step changes
{
	UPDATE repack_hi SET i = 100 WHERE i = 1;
	INSERT INTO repack_hi VALUES (1, 1001, 'new');
	UPDATE repack_hi SET j = 2001 WHERE i = 1;
	DELETE FROM repack_hi WHERE i = 2;
	UPDATE repack_hi SET i = 101 WHERE i = 100;
}
step wakeup
{
	SELECT injection_points_wakeup('repack-concurrently-before-lock');
}

permutation repack changes wakeup check
