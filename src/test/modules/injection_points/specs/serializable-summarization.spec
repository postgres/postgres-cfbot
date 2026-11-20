# Test SSI conflict detection after summarization.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE summarized_write_skew (id int PRIMARY KEY, v int);
	INSERT INTO summarized_write_skew VALUES (1, 0), (2, 0);
}

teardown
{
	DROP TABLE summarized_write_skew;
	DROP EXTENSION injection_points;
}

session s1
step s1_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s1_read	{ SELECT count(*) FROM summarized_write_skew WHERE v = 1; }
step s1_write
{
	UPDATE summarized_write_skew
	SET v = 1
	WHERE id = 2
	  AND (SELECT count(*) FROM summarized_write_skew WHERE v = 1) = 0;
}
step s1_commit	{ COMMIT; }

session s2
step s2_begin	{ BEGIN ISOLATION LEVEL SERIALIZABLE; }
step s2_read	{ SELECT count(*) FROM summarized_write_skew WHERE v = 1; }
step s2_write
{
	UPDATE summarized_write_skew
	SET v = 1
	WHERE id = 1
	  AND (SELECT count(*) FROM summarized_write_skew WHERE v = 1) = 0;
}
step s2_commit	{ COMMIT; }

session s3
step attach
{
	SELECT injection_points_attach('serializable-summarize', 'notice');
}
step summarize
{
	BEGIN ISOLATION LEVEL SERIALIZABLE;
	SELECT 1;
	COMMIT;
}
step detach
{
	SELECT count(*) > 0 AS locks_summarized
	FROM pg_locks
	WHERE mode = 'SIReadLock' AND virtualtransaction = '-1/0';
	SELECT injection_points_detach('serializable-summarize');
}

# The ordinary, unsummarized conflict is a control case.
permutation
	s1_begin s1_read
	s2_begin s2_read s2_write s2_commit
	s1_write s1_commit

# Moving s2's SIREAD locks to OldCommittedSxact must preserve the conflict.
permutation
	s1_begin s1_read
	s2_begin s2_read s2_write s2_commit
	attach summarize detach
	s1_write s1_commit
