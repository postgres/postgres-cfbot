# Test post-lock publication ownership checks in ALTER PUBLICATION.
#
# Session s1 holds the publication object lock with COMMENT ON PUBLICATION,
# then changes the owner in the same transaction. Session s2 sees the old
# owner and waits for the object lock. Once s1 commits, s2 must recheck the
# publication state and reject the former owner.

setup
{
	CREATE ROLE regress_pub_owner1;
	CREATE ROLE regress_pub_owner2;
	CREATE TABLE regress_pub_owner_lock_table (a int);
	ALTER TABLE regress_pub_owner_lock_table OWNER TO regress_pub_owner1;
	CREATE PUBLICATION regress_pub_owner_lock;
	ALTER PUBLICATION regress_pub_owner_lock OWNER TO regress_pub_owner1;
}

teardown
{
	DROP PUBLICATION regress_pub_owner_lock;
	DROP TABLE regress_pub_owner_lock_table;
	DROP ROLE regress_pub_owner1;
	DROP ROLE regress_pub_owner2;
}

session s1
step s1_begin		{ BEGIN; }
step s1_lock		{ COMMENT ON PUBLICATION regress_pub_owner_lock IS 'locked'; }
step s1_alter_owner	{ ALTER PUBLICATION regress_pub_owner_lock OWNER TO regress_pub_owner2; }
step s1_commit		{ COMMIT; }

session s2
step s2_set_role	{ SET ROLE regress_pub_owner1; }
step s2_alter		{ ALTER PUBLICATION regress_pub_owner_lock ADD TABLE regress_pub_owner_lock_table; }
step s2_reset_role	{ RESET ROLE; }

permutation s1_begin s1_lock s1_alter_owner s2_set_role s2_alter s1_commit s2_reset_role
