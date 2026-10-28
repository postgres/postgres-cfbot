# REINDEX CONCURRENTLY of a TOAST index
#
# Verify that a backend writing to a TOAST table starts inserting into the
# new TOAST index once it is marked ready, so that no chunk is missing after
# the rebuild.
#
# An injection point fires after the TOAST indexes are opened.

setup
{
    CREATE EXTENSION injection_points;
    CREATE TABLE reind_toast (id int, val text);
    ALTER TABLE reind_toast ALTER COLUMN val SET STORAGE EXTERNAL;
    INSERT INTO reind_toast VALUES (1, repeat('x', 10000));
    CREATE FUNCTION wait_for_point(text) RETURNS void LANGUAGE plpgsql AS $$
    BEGIN
        LOOP
            PERFORM 1 FROM pg_stat_activity WHERE wait_event = $1;
            EXIT WHEN FOUND;
            PERFORM pg_sleep(.1);
        END LOOP;
    END $$;
}

teardown
{
    DROP TABLE reind_toast;
    DROP FUNCTION wait_for_point;
    DROP EXTENSION injection_points;
}

# The rebuild, stopped once the new index is ready for inserts and again
# once it has been validated, before the swap.
session s1
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('reindex-conc-index-built', 'wait');
    SELECT injection_points_attach('reindex-relation-concurrently-before-swap', 'wait');
}
step reindex	{ REINDEX TABLE CONCURRENTLY reind_toast; }
step reindexed	{ }

# The writer, stopped with the TOAST indexes open.
session s2
setup
{
    SELECT injection_points_set_local();
    SELECT injection_points_attach('toast-save-datum-indexes-opened', 'wait');
}
step ins1	{ INSERT INTO reind_toast VALUES (2, repeat('y', 10000)); }
step inserted	{ }
# Written after the validation, so only a maintained index has it.
step ins2	{ INSERT INTO reind_toast VALUES (3, repeat('z', 10000)); }
step read	{ SELECT id, length(val) FROM reind_toast ORDER BY id; }

# Holds the rebuild back until the writer has the TOAST indexes open.
session s3
step lock	{ BEGIN; LOCK TABLE reind_toast IN ROW EXCLUSIVE MODE; }
step unlock	{ COMMIT; }
# A session that has been woken can still be reported as waiting on the
# point it came from, so wait for the name of the next one instead.
step await_ready	{ SELECT wait_for_point('reindex-conc-index-built'); }
step wake_ins
{
    SELECT injection_points_detach('toast-save-datum-indexes-opened');
    SELECT injection_points_wakeup('toast-save-datum-indexes-opened');
}
step wake_ready
{
    SELECT injection_points_detach('reindex-conc-index-built');
    SELECT injection_points_wakeup('reindex-conc-index-built');
}
step await_swap	{ SELECT wait_for_point('reindex-relation-concurrently-before-swap'); }
step wake_swap
{
    SELECT injection_points_detach('reindex-relation-concurrently-before-swap');
    SELECT injection_points_wakeup('reindex-relation-concurrently-before-swap');
}

permutation lock reindex ins1 unlock await_ready wake_ins inserted wake_ready await_swap ins2 wake_swap reindexed read
