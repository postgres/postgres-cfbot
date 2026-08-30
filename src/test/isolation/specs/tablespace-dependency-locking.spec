# Test that concurrent DROP TABLESPACE and CREATE TABLE do not leave behind
# references to a non-existent tablespace.

setup
{
	SET allow_in_place_tablespaces = true;
}

setup
{
	CREATE TABLESPACE regress_dependency_tablespace LOCATION '';
}

teardown
{
	DROP TABLESPACE IF EXISTS regress_dependency_tablespace;
}

session "s1"

step "s1_begin" { BEGIN; }
step "s1_create_table_in_tablespace"
{
	CREATE TABLE tbl_tablespace (a int) PARTITION BY RANGE (a)
		TABLESPACE regress_dependency_tablespace;
}
step "s1_alter_tablespace"
{
	ALTER TABLESPACE regress_dependency_tablespace
		SET (random_page_cost = 1.1);
}
step "s1_commit" { COMMIT; }
step "s1_rollback" { ROLLBACK; }
step "s1_drop_table" { DROP TABLE tbl_tablespace; }
step "s1_drop_tablespace" { DROP TABLESPACE regress_dependency_tablespace; }

teardown
{
	SET client_min_messages = warning;
	DROP TABLE IF EXISTS tbl_tablespace;
}

session "s2"

step "s2_drop_tablespace" { DROP TABLESPACE regress_dependency_tablespace; }

session "s3"

step "s3_create_table_in_dropped_tablespace"
{
	DO $$
	BEGIN
		EXECUTE 'CREATE TABLE tbl_tablespace (a int) PARTITION BY RANGE (a)
			TABLESPACE regress_dependency_tablespace';
	EXCEPTION WHEN undefined_object THEN
		RAISE NOTICE 'referenced tablespace was concurrently dropped';
	END
	$$;
}

# create table - drop tablespace
permutation "s1_begin" "s1_create_table_in_tablespace" "s2_drop_tablespace" "s1_commit" "s1_drop_table" "s1_drop_tablespace"

# drop tablespace - create table; ALTER makes DROP wait while deleting the
# catalog tuple, after DROP has checked for dependencies
permutation "s1_begin" "s1_alter_tablespace" "s2_drop_tablespace" "s3_create_table_in_dropped_tablespace" "s1_rollback"
