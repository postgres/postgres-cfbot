# Test progress reporting of subcommands.
setup
{
        CREATE EXTENSION injection_points;
        CREATE TABLE progress_test(i int primary key);
}

teardown
{
        DROP TABLE progress_test;
        DROP EXTENSION injection_points;
}

session s1
setup
{
        SELECT injection_points_set_local();
}
# REPACK is suitable because it reports progress itself and it also runs
# another progress reporting command (CREATE_INDEX).
step repack_wait
{
        SELECT injection_points_attach('progress-end-command-nested', 'wait');
        REPACK progress_test;
}

session s2
step check_index
{
        SELECT command FROM pg_stat_progress_create_index;
}
step wakeup
{
        SELECT injection_points_wakeup('progress-end-command-nested');
}

session s3
setup
{
        SELECT injection_points_set_local();
}
step repack_error
{
        SELECT injection_points_attach('progress-update-param-nested', 'error');
        REPACK progress_test;
}

session s4
step check_repack
{
        SELECT command FROM pg_stat_progress_repack;
}


# Wait just before ending the subcommand's progress reporting and check that
# it's the index creation.
permutation
repack_wait
check_index
wakeup

# If a subcommand ends up with ERROR, the backend's progress reporting state
# must be cleaned up properly.
permutation
repack_error
check_repack
