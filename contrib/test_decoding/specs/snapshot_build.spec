# Test snapshot build correctly, it must track committed transactions during BUILDING_SNAPSHOT

setup
{
    DROP TABLE IF EXISTS tbl1;
    DROP TABLE IF EXISTS tbl2;
    CREATE TABLE tbl1 (val1 integer);
}

teardown
{
    DROP TABLE tbl1;
    DROP TABLE tbl2;
    SELECT 'stop' FROM pg_drop_replication_slot('isolation_slot');
}

session "s1"
setup { SET synchronous_commit=on; }
step "s1_begin" { BEGIN; }
step "s1_insert" { INSERT INTO tbl1 VALUES (1); }
step "s1_commit" { COMMIT; }

session "s2"
setup { SET synchronous_commit=on; }
step "s2_init" { SELECT 'init' FROM pg_create_logical_replication_slot('isolation_slot', 'test_decoding'); }
step "s2_get_changes" { SELECT data FROM pg_logical_slot_get_changes('isolation_slot', NULL, NULL, 'skip-empty-xacts', '1', 'include-xids', '0'); }

session "s3"
setup { SET synchronous_commit=on; }
step "s3_begin" { BEGIN; }
step "s3_insert" { INSERT INTO tbl1 VALUES (1); }
step "s3_commit" { COMMIT; }

session "s4"
setup { SET synchronous_commit=on; }
step "s4_create" { CREATE TABLE tbl2 (val1 integer); }
step "s4_begin" { BEGIN; }
step "s4_insert" { INSERT INTO tbl2 VALUES (1); }
step "s4_commit" { COMMIT; }

# T1: s1_begin -> s1_insert -> BUILDING_SNAPSHOT -> s1_commit -> FULL_SNAPSHOT
# T2: BUILDING_SNAPSHOT -> s3_begin -> s3_insert -> FULL_SNAPSHOT -> s3_commit -> CONSISTENT
# T3: BUILDING_SNAPSHOT -> s4_create -> FULL_SNAPSHOT
# T4: FULL_SNAPSHOT -> s4_begin -> s4_insert -> CONSISTENT -> s4_commit
# The snapshot must track T3 or the replay of T4 will fail because its snapshot cannot see tbl2
permutation "s1_begin" "s1_insert" "s2_init" "s3_begin" "s3_insert" "s4_create" "s1_commit" "s4_begin" "s4_insert" "s3_commit" "s4_commit" "s2_get_changes"
