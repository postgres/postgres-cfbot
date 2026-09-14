# FLUX: a heavyweight tuple lock must be keyed by the relation's pg_class OID,
# as heap's LockTuple is (lmgr.c uses rd_lockInfo.lockRelId.relId), not by the
# relfilenode (rd_locator.relNumber).  The two counters share the global
# LOCKTAG_TUPLE/DEFAULT_LOCKMETHOD namespace, so after a rewrite -- which gives
# the relation a new relfilenode that differs from its OID and can equal an
# unrelated relation's OID -- a relfilenode-keyed tuple lock collides across
# relations (spurious cross-AM wait/deadlock) and pg_locks names the wrong
# relation.
#
# s0rw rewrites the table in its own transaction (a rewrite inside the CREATE
# transaction would reuse the relfilenode), so relfilenode <> OID.  The waiter
# s2 takes the heavyweight tuple lock while it blocks on s1's row lock
# (FluxLockTupleToWait); s1locks reads that lock out of pg_locks: its
# 'relation' field must resolve to the table 'tl', i.e. its pg_class OID.  On
# the buggy code the field is the relfilenode and does not name 'tl'.
setup
{
  CREATE TABLE tl (id int PRIMARY KEY, v int) USING flux;
  INSERT INTO tl VALUES (1, 0);
}
teardown
{
  DROP TABLE tl;
}

session s0
# A rewrite in its own transaction gives tl a relfilenode <> its OID.
step s0rw	{ VACUUM FULL tl; }

session s1
setup	{ BEGIN; }
step s1fu	{ SELECT id FROM tl WHERE id = 1 FOR UPDATE; }
# The tuple lock s2 holds while waiting must name 'tl' (its OID), not the
# relfilenode.  With the bug 'names_tl' is f (the OID did not resolve to tl).
step s1locks	{ SELECT relation = 'tl'::regclass AS names_tl
		  FROM pg_locks WHERE locktype = 'tuple'; }
step s1c	{ COMMIT; }

session s2
step s2fu	{ SELECT id FROM tl WHERE id = 1 FOR UPDATE; }

# Rewrite tl; s1 holds the row; s2 blocks on it and takes the heavyweight
# tuple lock while waiting; s1 reads that lock's relation; s1 commits and s2
# proceeds.
permutation s0rw s1fu s2fu s1locks s1c
