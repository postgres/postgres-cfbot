# Test CREATE INDEX on a partitioned table concurrently with
# ALTER INDEX ... ATTACH PARTITION on one of its partitions' indexes.
#
# CREATE INDEX must notice that the partition's index was attached to
# another parent while it waited for the lock on it, and must not wait for
# locks on partition indexes that were already attached.

setup
{
  CREATE TABLE pia (a int) PARTITION BY RANGE (a);
  CREATE TABLE pia_1 PARTITION OF pia FOR VALUES FROM (0) TO (10);
  CREATE TABLE pia_2 PARTITION OF pia FOR VALUES FROM (10) TO (20);
  CREATE INDEX pia_old ON ONLY pia (a);
  CREATE INDEX pia_1_a ON pia_1 (a);
  CREATE INDEX pia_2_a ON pia_2 (a);
  ALTER INDEX pia_old ATTACH PARTITION pia_2_a;
}

teardown
{
  DROP TABLE pia;
}

session s1
step s1b		{ BEGIN; }
step s1attach	{ ALTER INDEX pia_old ATTACH PARTITION pia_1_a; }
step s1rename	{ ALTER INDEX pia_2_a RENAME TO pia_2_renamed; }
step s1c		{ COMMIT; }

session s2
step s2create	{ CREATE INDEX pia_new ON pia (a); }
step s2check	{
  SELECT c.relname AS index, p.relname AS parent
  FROM pg_inherits i
    JOIN pg_class c ON c.oid = i.inhrelid
    JOIN pg_class p ON p.oid = i.inhparent
  WHERE p.relname IN ('pia_old', 'pia_new')
  ORDER BY 1;
}

# CREATE INDEX waits for the ATTACH, then must build a new index on pia_1
# rather than trying to reuse pia_1_a.
permutation s1b s1attach s2create s1c s2check

# CREATE INDEX must not wait for the lock on an already-attached index.
permutation s1b s1rename s2create s1c s2check
