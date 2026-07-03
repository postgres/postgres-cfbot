# Test post-lock subscription checks in ALTER and DROP SUBSCRIPTION.
#
# In the first two permutations, session s1 holds the subscription object lock
# with COMMENT ON SUBSCRIPTION, then changes the owner in the same transaction.
# Session s2 sees the old owner and waits for the object lock. Once s1 commits,
# s2 must recheck the subscription state and reject the former owner.
#
# The last two permutations cover concurrent DROP separately. Session s1
# deletes the subscription but leaves the transaction open, so session s2 can
# resolve the old name before waiting for the object lock. After s1 commits,
# s2 must process the invalidation, recheck the subscription state, and report
# either ERROR or NOTICE according to whether IF EXISTS was specified.

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
	DROP SUBSCRIPTION IF EXISTS regress_sub_owner_lock;
	DROP ROLE regress_sub_owner1;
	DROP ROLE regress_sub_owner2;
}

session s1
step s1_begin		{ BEGIN; }
step s1_lock		{ COMMENT ON SUBSCRIPTION regress_sub_owner_lock IS 'locked'; }
step s1_alter_owner	{ ALTER SUBSCRIPTION regress_sub_owner_lock OWNER TO regress_sub_owner2; }
step s1_drop		{ DROP SUBSCRIPTION regress_sub_owner_lock; }
step s1_commit		{ COMMIT; }

session s2
step s2_set_role		{ SET ROLE regress_sub_owner1; }
step s2_alter			{ ALTER SUBSCRIPTION regress_sub_owner_lock SET (synchronous_commit = local); }
step s2_drop			{ DROP SUBSCRIPTION regress_sub_owner_lock; }
step s2_drop_if_exists	{ DROP SUBSCRIPTION IF EXISTS regress_sub_owner_lock; }
step s2_reset_role		{ RESET ROLE; }

permutation s1_begin s1_lock s1_alter_owner s2_set_role s2_alter s1_commit s2_reset_role
permutation s1_begin s1_lock s1_alter_owner s2_set_role s2_drop s1_commit s2_reset_role

# The second DROP must recheck the subscription after waiting and honor
# IF EXISTS if the first DROP removed it.
permutation s1_begin s1_drop s2_drop s1_commit
permutation s1_begin s1_drop s2_drop_if_exists s1_commit
