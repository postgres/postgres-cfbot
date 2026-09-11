# Test lazy snapshot distribution: snapshots are no longer eagerly distributed
# to all in-progress transactions on each catalog-modifying commit. Instead,
# they are lazily distributed when a transaction actually decodes a data change.
# This avoids O(N^2) snapshot disk usage when a long-running transaction
# coexists with many catalog-modifying commits (e.g. autovacuum).

setup
{
    SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding');
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS tbl2;
    DROP TABLE IF EXISTS dummy1;
    DROP TABLE IF EXISTS dummy2;
    DROP TABLE IF EXISTS dummy3;
    CREATE TABLE tbl1 (val1 integer);
    CREATE TABLE tbl2 (val1 integer);
}

teardown
{
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS tbl2;
    DROP TABLE IF EXISTS dummy1;
    DROP TABLE IF EXISTS dummy2;
    DROP TABLE IF EXISTS dummy3;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

# Session s1: long-running transaction
session "s1"
setup { SET synchronous_commit=on; }
step "s1_begin" { BEGIN; }
step "s1_insert_tbl1" { INSERT INTO tbl1 VALUES (1); }
step "s1_insert_tbl2_3col" { INSERT INTO tbl2 VALUES (1, 10, 100); }
step "s1_savepoint" { SAVEPOINT sp1; }
step "s1_release" { RELEASE SAVEPOINT sp1; }
step "s1_commit" { COMMIT; }

# Session s2: performs catalog-modifying operations
session "s2"
setup { SET synchronous_commit=on; }
step "s2_add_col_c1" { ALTER TABLE tbl2 ADD COLUMN c1 integer; }
step "s2_add_col_c2" { ALTER TABLE tbl2 ADD COLUMN c2 integer; }
step "s2_create_dummy1" { CREATE TABLE dummy1 (id int); }
step "s2_create_dummy2" { CREATE TABLE dummy2 (id int); }
step "s2_create_dummy3" { CREATE TABLE dummy3 (id int); }

# Session s3: consumes changes
session "s3"
setup { SET synchronous_commit=on; }
step "s3_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'include-xids', '0', 'skip-empty-xacts', '1'); }

# Scenario 1: Long transaction + multiple DDLs + correct decoding
#
# s1 starts a long transaction and inserts into tbl1 (gets base snapshot).
# s2 performs two ALTER TABLE ADD COLUMN on tbl2.
# s1 then inserts into tbl2 using the new schema (3 columns).
# The lazily distributed snapshot at s1's second insert must reflect both
# ALTER TABLEs so the new columns are visible during decoding.
permutation "s1_begin" "s1_insert_tbl1" "s2_add_col_c1" "s2_add_col_c2" "s1_insert_tbl2_3col" "s1_commit" "s3_get_changes"

# Scenario 2: Long transaction + many catalog changes + correct decoding
#
# s1 starts a long transaction and inserts into tbl1.
# s2 performs multiple catalog-modifying DDLs (CREATE TABLE), simulating
# the pattern of autovacuum generating many catalog change commits.
# s1 inserts again into tbl1 after all the DDLs.
# Both inserts must be correctly decoded despite many catalog-modifying
# commits in between.  With eager distribution, each CREATE TABLE would
# have distributed a snapshot to s1; with lazy distribution, only the
# last snapshot is distributed when s1 does its second insert.
permutation "s1_begin" "s1_insert_tbl1" "s2_create_dummy1" "s2_create_dummy2" "s2_create_dummy3" "s1_insert_tbl1" "s1_commit" "s3_get_changes"

# Scenario 3: Subtransaction + catalog change + correct decoding
#
# s1 starts a transaction with a savepoint, inserts into tbl1 inside the
# subtransaction.  s2 performs ALTER TABLE ADD COLUMN on tbl2.  s1 then
# inserts into tbl2 with the new column, still inside the subtransaction.
# Both inserts must decode correctly, verifying lazy distribution works
# with subtransactions.
permutation "s1_begin" "s1_savepoint" "s1_insert_tbl1" "s2_add_col_c1" "s2_add_col_c2" "s1_insert_tbl2_3col" "s1_release" "s1_commit" "s3_get_changes"
