# Test that ALTER SUBSCRIPTION rechecks ownership after waiting for the
# subscription object lock.
#
# Session s1 holds the subscription object lock with COMMENT ON SUBSCRIPTION,
# then changes the owner in the same transaction. Session s2 sees the old
# owner and waits for the object lock. Once s1 commits, s2 must recheck the
# subscription state and reject the former owner.

setup
{
	CREATE ROLE regress_sub_owner1;
	CREATE ROLE regress_sub_owner2;
	CREATE SUBSCRIPTION regress_sub_owner_lock
		CONNECTION '' PUBLICATION regress_pub
		WITH (connect = false, slot_name = NONE);
	ALTER SUBSCRIPTION regress_sub_owner_lock OWNER TO regress_sub_owner1;
}

teardown
{
	DROP SUBSCRIPTION regress_sub_owner_lock;
	DROP ROLE regress_sub_owner1;
	DROP ROLE regress_sub_owner2;
}

session s1
step s1_begin		{ BEGIN; }
step s1_lock		{ COMMENT ON SUBSCRIPTION regress_sub_owner_lock IS 'locked'; }
step s1_alter_owner	{ ALTER SUBSCRIPTION regress_sub_owner_lock OWNER TO regress_sub_owner2; }
step s1_commit		{ COMMIT; }

session s2
step s2_set_role	{ SET ROLE regress_sub_owner1; }
step s2_alter		{ ALTER SUBSCRIPTION regress_sub_owner_lock SET (synchronous_commit = local); }
step s2_reset_role	{ RESET ROLE; }

permutation s1_begin s1_lock s1_alter_owner s2_set_role s2_alter s1_commit s2_reset_role
