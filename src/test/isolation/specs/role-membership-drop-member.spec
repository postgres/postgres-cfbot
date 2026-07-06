# Test that role membership commands properly lock the grantee/member
# role to prevent concurrent DROP ROLE from creating orphaned pg_auth_members
# entries, or from operating on a stale OID.

setup
{
	CREATE ROLE regress_role_group;
	CREATE ROLE regress_role_member;
	CREATE ROLE regress_role_group_cu;
	CREATE ROLE regress_role_member_cu;
	GRANT regress_role_group_cu TO regress_role_member_cu WITH ADMIN OPTION;
}

teardown
{
	DROP ROLE IF EXISTS regress_role_group;
	DROP ROLE IF EXISTS regress_role_member;
	DROP ROLE IF EXISTS regress_role_new;
	DROP ROLE IF EXISTS regress_role_member_cu;
	DROP ROLE IF EXISTS regress_role_group_cu;
}

session s1
step s1_begin		{ BEGIN; }
step s1_grant		{ GRANT regress_role_group TO regress_role_member; }
step s1_alter_add	{ ALTER GROUP regress_role_group ADD USER regress_role_member; }
step s1_create_role	{ CREATE ROLE regress_role_new ROLE regress_role_member; }
step s1_drop_owned	{ DROP OWNED BY regress_role_member; }
step s1_reassign_owned	{ REASSIGN OWNED BY regress_role_member TO regress_role_group; }
step s1_set_role	{ SET ROLE regress_role_member_cu; }
step s1_grant_cu	{ GRANT regress_role_group_cu TO CURRENT_USER; }
step s1_commit		{ COMMIT; }
step s1_reset		{ RESET ROLE; }

session s2
step s2_drop_member	{ DROP ROLE regress_role_member; }
step s2_drop_member_cu	{ DROP ROLE regress_role_member_cu; }
step s2_check_orphans	{
	SELECT count(*)
	FROM pg_auth_members m
	LEFT JOIN pg_authid ra ON m.roleid  = ra.oid
	LEFT JOIN pg_authid me ON m.member  = me.oid
	LEFT JOIN pg_authid gr ON m.grantor = gr.oid
	WHERE ra.oid IS NULL OR me.oid IS NULL OR gr.oid IS NULL;
}

# GRANT role TO member and concurrent DROP of the member
permutation s1_begin s1_grant s2_drop_member s1_commit s2_check_orphans

# ALTER GROUP ADD USER and concurrent DROP of the member
permutation s1_begin s1_alter_add s2_drop_member s1_commit s2_check_orphans

# CREATE ROLE ... ROLE member and concurrent DROP of the member
permutation s1_begin s1_create_role s2_drop_member s1_commit s2_check_orphans

# DROP OWNED BY role and concurrent DROP of the role
permutation s1_begin s1_drop_owned s2_drop_member s1_commit

# REASSIGN OWNED BY role and concurrent DROP of the role
permutation s1_begin s1_reassign_owned s2_drop_member s1_commit

# GRANT role TO CURRENT_USER and concurrent DROP of the current user
permutation s1_begin s1_set_role s1_grant_cu s2_drop_member_cu s1_commit s1_reset s2_check_orphans
