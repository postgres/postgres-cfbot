# Verify that ALTER TABLE ... ATTACH PARTITION locks all ancestors of the
# target table, from the root down, before locking the target itself.
#
# ATTACH PARTITION needs the partition constraint of its target, which
# generate_partition_qual() builds by walking up the partition tree and
# locking each ancestor.  A recursing ALTER TABLE on the root locks the tree
# from the top down, so if ATTACH locked the target before its ancestors,
# the two could deadlock.  Three levels are used so that locking only the
# target's direct parent first would not be enough.

setup
{
  CREATE TABLE attach_lock_root (id int) PARTITION BY RANGE (id);
  CREATE TABLE attach_lock_mid1 PARTITION OF attach_lock_root FOR VALUES FROM (0) TO (1000) PARTITION BY RANGE (id);
  CREATE TABLE attach_lock_mid2 PARTITION OF attach_lock_mid1 FOR VALUES FROM (0) TO (500) PARTITION BY RANGE (id);
  CREATE TABLE attach_lock_new (id int);
}

teardown
{
  DROP TABLE attach_lock_new;
  DROP TABLE attach_lock_root;
}

# ALTER TABLE ... SET DEFAULT on the root locks the root and then recurses to
# its partitions.  We can simulate ATTACH PARTITION starting between those
# steps by locking only the root beforehand.
session s1
step s1begin	{ BEGIN; }
step s1lock		{ LOCK TABLE ONLY attach_lock_root IN ACCESS EXCLUSIVE MODE; }
step s1alter	{ ALTER TABLE attach_lock_root ALTER COLUMN id SET DEFAULT 0; }
step s1commit	{ COMMIT; }

session s2
step s2attach	{ ALTER TABLE attach_lock_mid2 ATTACH PARTITION attach_lock_new FOR VALUES FROM (0) TO (100); }

# s2attach must wait for the root lock without holding a lock on any of the
# root's partitions, so that s1alter can complete.
permutation s1begin s1lock s2attach s1alter s1commit
