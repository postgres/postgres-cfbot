# Copyright (c) 2024-2026, PostgreSQL Global Development Group

"""Port of src/test/recovery/t/049_wait_for_lsn.pl.

Exercises the WAIT FOR LSN command: the standby_replay/standby_write/
standby_flush modes on a standby and primary_flush on a primary, timeouts and
no_throw status reporting, subtransaction cleanup, parameter/mode validation,
many concurrent waiters per mode, promotion terminating standby waiters,
archive-only standbys (replay-position floor), fresh-shmem walreceiver startup,
off-by-one fencepost boundaries, re-registration after a stale wakeup, a
timeline switch on a cascade standby, and implicit segment flushes waking
primary_flush waiters.

The Perl test detects waiter completion by logging from a follow-up function;
here a blocked waiter is dispatched with ``background_sql()`` and its completion
observed by resolving the returned future, so the log helpers are unnecessary.
"""

import pytest
from libpq import LibpqError
from pypg import require_extension, require_injection_points

# Helper functions to control walreceiver for testing wait conditions. These
# allow us to stop WAL streaming so waiters actually block, then restart it.


def stop_walreceiver(node):
    """Stop the walreceiver on ``node`` by clearing primary_conninfo and waiting
    until pg_stat_wal_receiver becomes empty. Returns the previous (quoted)
    value, for resume_walreceiver().
    """
    saved = node.sql(
        "SELECT pg_catalog.quote_literal(setting) FROM pg_settings "
        "WHERE name = 'primary_conninfo'"
    )
    node.sql("ALTER SYSTEM SET primary_conninfo = ''")
    node.sql("SELECT pg_reload_conf()")

    node.poll_query_until("SELECT NOT EXISTS (SELECT * FROM pg_stat_wal_receiver)")
    return saved


def resume_walreceiver(node, saved):
    """Restart the walreceiver on ``node`` by restoring primary_conninfo to the
    value captured by stop_walreceiver(), and waiting until it reconnects.
    """
    node.sql(f"ALTER SYSTEM SET primary_conninfo = {saved}")
    node.sql("SELECT pg_reload_conf()")

    node.poll_query_until("SELECT EXISTS (SELECT * FROM pg_stat_wal_receiver)")


def check_fencepost(node, mode, current_lsn, label):
    """Verify the wait predicate "target <= currentLSN" at the boundary.

    Given ``current_lsn`` (the frozen position for ``mode``), current and
    current-1 must succeed and current+1 must time out. Returns the two
    neighbouring LSNs, so the caller can reuse them.
    """
    lsn_minus = node.sql("SELECT ($1::pg_lsn - 1)::text", current_lsn)
    lsn_plus = node.sql("SELECT ($1::pg_lsn + 1)::text", current_lsn)
    for target, expected, timeout in (
        (current_lsn, "success", "5s"),
        (lsn_minus, "success", "5s"),
        (lsn_plus, "timeout", "500ms"),
    ):
        out = node.sql(
            f"WAIT FOR LSN '{target}' WITH (MODE '{mode}', timeout '{timeout}', no_throw)"
        )

        assert out == expected, f"{label}: target {target} expected {expected}"
    return lsn_minus, lsn_plus


def test_wait_for_lsn(create_pg):
    """A streaming standby: the wait modes, timeouts and status reporting,
    subtransaction cleanup, parameter and mode validation, many concurrent
    waiters per mode, and promotion terminating the waiters.

    These run against one primary/standby pair in sequence, and have to: the
    visibility check builds on the rows the previous section inserted, and the
    promotion at the end leaves the standby no longer a standby."""
    # Initialize primary node
    primary = create_pg("primary", allows_streaming=True)

    # And some content and take a backup
    primary.sql("CREATE TABLE wait_test AS SELECT generate_series(1,10) AS a")
    backup = primary.backup("my_backup")

    # Create a streaming standby with a 1 second delay from the backup
    standby = create_pg(
        "standby",
        from_backup=backup,
        streaming_primary=primary,
        start=False,
        conf={"recovery_min_apply_delay": "1s"},
    )
    standby.start()

    def insert_lsn(values):
        primary.sql(f"INSERT INTO wait_test VALUES ({values})")
        return primary.sql("SELECT pg_current_wal_insert_lsn()")

    # 1. Make sure that WAIT FOR works: add new content to
    # primary and memorize primary's insert LSN, then wait for that LSN to be
    # replayed on standby.
    lsn1 = insert_lsn("generate_series(11, 20)")
    standby.sql(f"WAIT FOR LSN '{lsn1}' WITH (timeout '1d')")

    # Make sure the current LSN on standby is at least as big as the LSN we
    # observed on primary's before.
    assert standby.sql("SELECT pg_lsn_cmp(pg_last_wal_replay_lsn(), $1)", lsn1) >= 0, (
        "standby reached the same LSN as primary after WAIT FOR"
    )

    # 2. Check that new data is visible after calling WAIT FOR
    lsn2 = insert_lsn("generate_series(21, 30)")
    standby.sql(f"WAIT FOR LSN '{lsn2}'")

    # Make sure the count(*) on standby reflects the recent changes on primary
    assert standby.sql("SELECT count(*) FROM wait_test") == 30, (
        "standby reached the same LSN as primary"
    )

    # 3. Check that WAIT FOR works with standby_write, standby_flush, and
    # primary_flush modes.
    lsn_write = insert_lsn("generate_series(31, 40)")
    standby.sql(f"WAIT FOR LSN '{lsn_write}' WITH (MODE 'standby_write', timeout '1d')")

    assert (
        standby.sql(
            "SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), $1)",
            lsn_write,
        )
        >= 0
    ), "standby wrote WAL up to target LSN with MODE 'standby_write'"

    lsn_flush = insert_lsn("generate_series(41, 50)")
    standby.sql(f"WAIT FOR LSN '{lsn_flush}' WITH (MODE 'standby_flush', timeout '1d')")

    assert (
        standby.sql("SELECT pg_lsn_cmp(pg_last_wal_receive_lsn(), $1)", lsn_flush) >= 0
    ), "standby flushed WAL up to target LSN with MODE 'standby_flush'"

    # Check primary_flush mode on primary
    lsn_primary_flush = insert_lsn("generate_series(51, 60)")
    primary.sql(
        f"WAIT FOR LSN '{lsn_primary_flush}' WITH (MODE 'primary_flush', timeout '1d')"
    )

    assert (
        primary.sql(
            "SELECT pg_lsn_cmp(pg_current_wal_flush_lsn(), $1)",
            lsn_primary_flush,
        )
        >= 0
    ), "primary flushed WAL up to target LSN with MODE 'primary_flush'"

    # 4. Check that waiting for unreachable LSN triggers the timeout.  The
    # unreachable LSN must be well in advance.  So WAL records issued by
    # the concurrent autovacuum could not affect that.
    lsn3 = primary.sql("SELECT pg_current_wal_insert_lsn() + 10000000000")
    standby.sql(f"WAIT FOR LSN '{lsn2}' WITH (timeout '10ms')")
    with pytest.raises(LibpqError, match="timed out while waiting for target LSN"):
        standby.sql(f"WAIT FOR LSN '{lsn3}' WITH (timeout '1000ms')")
    assert (
        standby.sql(f"WAIT FOR LSN '{lsn2}' WITH (timeout '0.1s', no_throw)")
        == "success"
    ), "WAIT FOR returns correct status after successful waiting"

    assert (
        standby.sql(f"WAIT FOR LSN '{lsn3}' WITH (timeout '10ms', no_throw)")
        == "timeout"
    ), "WAIT FOR returns correct status after timeout"

    # 4a. Check that aborting a subtransaction during WAIT FOR LSN cleans up the
    # shared wait-state.  Poll pg_stat_activity before canceling the first WAIT
    # FOR to ensure that the backend has registered itself in the waiters heap.
    # After rolling back to the savepoint, a second WAIT FOR in the same backend
    # must be able to register itself again.
    subxact_lsn = primary.sql("SELECT pg_current_wal_insert_lsn() + 10000000000")
    sub = primary.connect()
    sub.sql("SET application_name = 'wait_for_lsn_subxact_cleanup'")
    sub.sql("BEGIN")
    sub.sql("SAVEPOINT wait_cleanup")
    blocked = sub.background_sql(
        f"WAIT FOR LSN '{subxact_lsn}' WITH (MODE 'primary_flush')"
    )

    primary.poll_query_until(
        "SELECT count(*) = 1 FROM pg_stat_activity "
        "WHERE application_name = 'wait_for_lsn_subxact_cleanup' "
        "AND wait_event = 'WaitForWalFlush'"
    )

    assert primary.sql(
        "SELECT pg_cancel_backend(pid) FROM pg_stat_activity "
        "WHERE application_name = 'wait_for_lsn_subxact_cleanup' "
        "AND wait_event = 'WaitForWalFlush'"
    ), "canceled WAIT FOR LSN in subtransaction"
    with pytest.raises(LibpqError, match="canceling statement due to user request"):
        blocked.result()
    sub.sql("ROLLBACK TO wait_cleanup")

    assert (
        sub.sql(
            f"WAIT FOR LSN '{subxact_lsn}' WITH (MODE 'primary_flush', timeout '10ms', no_throw)"
        )
        == "timeout"
    ), "second WAIT FOR LSN timed out after savepoint rollback"
    sub.sql("COMMIT")
    sub.close()

    # 5. Check mode validation: standby modes error on primary, primary mode errors
    # on standby, and primary_flush works on primary.  Also check that WAIT FOR
    # triggers an error if called within a function, procedure, anonymous DO block,
    # or inside a transaction with an isolation level higher than READ COMMITTED.
    # Test standby_flush on primary - should error
    with pytest.raises(LibpqError, match="recovery is not in progress"):
        primary.sql(f"WAIT FOR LSN '{lsn3}' WITH (MODE 'standby_flush')")

    # Test primary_flush on standby - should error
    with pytest.raises(LibpqError, match="recovery is in progress"):
        standby.sql(f"WAIT FOR LSN '{lsn3}' WITH (MODE 'primary_flush')")

    # A transaction-snapshot-mode transaction has no snapshot until its first
    # query, so WAIT FOR is allowed before that point.  The target is already
    # replayed here, so the command returns immediately.
    with standby.connect() as c:
        c.sql("BEGIN ISOLATION LEVEL REPEATABLE READ")
        assert c.sql(f"WAIT FOR LSN '{lsn2}'") == "success", (
            "WAIT FOR is allowed before the transaction snapshot is taken"
        )
        c.sql("COMMIT")

    snapshot_error = (
        "WAIT cannot be executed while the current transaction holds a snapshot"
    )
    with standby.connect() as c:
        c.sql("BEGIN ISOLATION LEVEL REPEATABLE READ")
        c.sql("SELECT 1")
        with pytest.raises(LibpqError, match=snapshot_error) as excinfo:
            c.sql(f"WAIT FOR LSN '{lsn3}'")
    assert "isolation level higher than READ COMMITTED, so it holds a snapshot" in (
        excinfo.value.detail or ""
    ), "the isolation level is given as the reason when it is the reason"

    # The same error at READ COMMITTED must not blame the isolation level.
    with standby.connect() as c:
        c.sql("BEGIN")
        c.sql("DECLARE c CURSOR FOR SELECT 1")
        with pytest.raises(LibpqError, match=snapshot_error) as excinfo:
            c.sql(f"WAIT FOR LSN '{lsn3}'")
    assert "isolation level" not in (excinfo.value.detail or ""), (
        "the isolation level is not blamed at READ COMMITTED"
    )

    # Test wrapping WAIT FOR into function, procedure, and anonymous DO block
    # -- should error
    primary.sql_batch(
        """
        CREATE FUNCTION pg_wal_replay_wait_wrap(target_lsn pg_lsn) RETURNS void AS $$
          BEGIN
            EXECUTE format('WAIT FOR LSN %L;', target_lsn);
          END
        $$ LANGUAGE plpgsql;
        """,
        """
        CREATE PROCEDURE pg_wal_replay_wait_proc(target_lsn pg_lsn) AS $$
          BEGIN
            EXECUTE format('WAIT FOR LSN %L;', target_lsn);
          END
        $$ LANGUAGE plpgsql;
        """,
    )

    primary.wait_for_catchup(standby)
    top_level = "WAIT can only be executed as a top-level statement"
    with pytest.raises(LibpqError, match=top_level):
        standby.sql("SELECT pg_wal_replay_wait_wrap($1)", lsn3)
    with pytest.raises(LibpqError, match=top_level):
        standby.sql("CALL pg_wal_replay_wait_proc($1)", lsn3)
    with pytest.raises(LibpqError, match=top_level):
        standby.sql(f"DO $$ BEGIN EXECUTE format('WAIT FOR LSN %L;', '{lsn3}'); END $$")

    # 6. Check parameter validation error cases on standby before promotion
    test_lsn = primary.sql("SELECT pg_current_wal_insert_lsn()")
    cases = [
        # Test negative timeout
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (timeout '-1000ms')",
            "timeout cannot be negative",
        ),
        # Test out of range timeout
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (timeout '2147483648ms')",
            'invalid timeout value: "2147483648ms"',
        ),
        # Test unknown parameter with WITH clause
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (unknown_param 'value')",
            'option "unknown_param" not recognized',
        ),
        # Test duplicate TIMEOUT parameter with WITH clause
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (timeout '1000', timeout '2000')",
            "conflicting or redundant options",
        ),
        # Test duplicate NO_THROW parameter with WITH clause
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (no_throw, no_throw)",
            "conflicting or redundant options",
        ),
        # Test syntax error - options without WITH keyword
        (f"WAIT FOR LSN '{test_lsn}' (timeout '100ms')", "syntax error"),
        # Test syntax error - missing LSN
        ("WAIT FOR TIMEOUT 1000", "syntax error"),
        # Test invalid LSN format
        ("WAIT FOR LSN 'invalid_lsn'", "invalid input syntax for type pg_lsn"),
        # Test invalid timeout format
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (timeout 'invalid')",
            "invalid timeout value",
        ),
        # Test WITH clause error case - invalid option
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (invalid_option 'value')",
            'option "invalid_option" not recognized',
        ),
        # Test invalid MODE value
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (MODE 'invalid')",
            'unrecognized value for WAIT option "mode": "invalid"',
        ),
        # Test duplicate MODE parameter
        (
            f"WAIT FOR LSN '{test_lsn}' WITH (MODE 'standby_replay', MODE 'standby_write')",
            "conflicting or redundant options",
        ),
    ]
    for query, msg in cases:
        with pytest.raises(LibpqError, match=msg):
            standby.sql(query)

    assert (
        standby.sql(f"WAIT FOR LSN '{lsn2}' WITH (timeout '0.1s', no_throw)")
        == "success"
    ), "WAIT FOR WITH clause syntax works correctly"

    assert (
        standby.sql(f"WAIT FOR LSN '{lsn3}' WITH (timeout 100, no_throw)") == "timeout"
    ), "WAIT FOR WITH clause returns correct timeout status"

    # Test maximum timeout
    assert (
        standby.sql(f"WAIT FOR LSN '{lsn2}' WITH (timeout '2147483647ms', no_throw)")
        == "success"
    ), "maximum timeout value is accepted"

    # An unsatisfied standby_replay wait must be rejected when the backend holds
    # a lock.  The relation lock covers the direct two-process cycle, while the
    # advisory lock is the return edge in the indirect three-process cycle.  The
    # short timeout keeps these tests bounded if the check regresses.
    for what, lock_query in (
        ("a relation lock", "SELECT count(*) FROM wait_test"),
        ("an advisory lock", "SELECT pg_advisory_xact_lock(42)"),
    ):
        with standby.connect() as c:
            c.sql("BEGIN")
            c.sql(lock_query)
            with pytest.raises(
                LibpqError, match="cannot wait for a standby LSN while holding locks"
            ):
                c.sql(f"WAIT FOR LSN '{lsn3}' WITH (timeout '100ms')")

    # A wait whose target has already been reached returns without sleeping, so it
    # cannot join a cycle and stays allowed regardless of the locks held.
    with standby.connect() as c:
        c.sql("BEGIN")
        c.sql("LOCK TABLE wait_test IN ACCESS SHARE MODE")
        assert c.sql(f"WAIT FOR LSN '{lsn1}'") == "success", (
            "allow an already-reached wait while holding a relation lock"
        )

    # 7a. Check the scenario of multiple standby_replay waiters.  We make 5
    # background psql sessions each waiting for a corresponding insertion.  When
    # waiting is finished, stored procedures logs if there are visible as many
    # rows as should be.
    standby.sql("SELECT pg_wal_replay_pause()")
    replay_waiters = []
    for i in range(5):
        lsn = insert_lsn(str(i))

        # Read the count from the primary rather than hardcoding it: the
        # sections above this one decide how many rows exist by now.
        expected_count = primary.sql("SELECT count(*) FROM wait_test")
        session = standby.connect()
        replay_waiters.append(
            (session, session.background_sql(f"WAIT FOR LSN '{lsn}'"), expected_count)
        )
    standby.sql("SELECT pg_wal_replay_resume()")
    for i, (session, fut, expected_count) in enumerate(replay_waiters):
        fut.result()

        assert session.sql("SELECT count(*) FROM wait_test") >= expected_count, (
            f"standby_replay waiter {i} sees the rows up to its target LSN"
        )
        session.close()

    def multi_mode_waiters(mode, base, wait_event):
        """7b/7c. Check the scenario of multiple standby_write and
        standby_flush waiters.  Stop walreceiver to ensure waiters actually
        block.
        """
        saved = stop_walreceiver(standby)

        # Generate WAL on primary (standby won't receive it yet)
        lsns = [insert_lsn(str(base + i)) for i in range(5)]

        # Start waiters (they will block since walreceiver is stopped)
        waiters = [
            standby.background_sql_oneshot(
                f"WAIT FOR LSN '{lsns[i]}' WITH (MODE '{mode}', timeout '1d')"
            )
            for i in range(5)
        ]

        # Verify waiters are blocked
        standby.poll_query_until(
            "SELECT count(*) = 5 FROM pg_stat_activity WHERE wait_event = $1",
            wait_event,
        )

        # Restore walreceiver to unblock waiters
        resume_walreceiver(standby, saved)

        # Wait for all waiters to complete and close sessions
        for fut in waiters:
            fut.result()
        return lsns

    write_lsns = multi_mode_waiters("standby_write", 100, "WaitForWalWrite")

    # Verify on standby that WAL was written up to the target LSN
    assert (
        standby.sql(
            "SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), $1)",
            write_lsns[4],
        )
        >= 0
    ), "multiple standby_write waiters: standby wrote WAL up to target LSN"

    flush_lsns = multi_mode_waiters("standby_flush", 200, "WaitForWalFlush")

    # Verify on standby that WAL was flushed up to the target LSN
    assert (
        standby.sql("SELECT pg_lsn_cmp(pg_last_wal_receive_lsn(), $1)", flush_lsns[4])
        >= 0
    ), "multiple standby_flush waiters: standby flushed WAL up to target LSN"

    # 7d. Check the scenario of mixed standby mode waiters (standby_replay,
    # standby_write, standby_flush) running concurrently.  We start 6 sessions:
    # 2 for each mode, all waiting for the same target LSN.  We stop the
    # walreceiver and pause replay to ensure all waiters block.  Then we resume
    # replay and restart the walreceiver to verify they unblock and complete
    # correctly.
    # Stop walreceiver first to ensure we can control the flow without hanging
    # (stopping it after pausing replay can hang).
    saved = stop_walreceiver(standby)

    # Pause replay
    standby.sql("SELECT pg_wal_replay_pause()")

    # Generate WAL on primary
    mixed_target = insert_lsn("generate_series(301, 310)")

    # Start 6 waiters: 2 for each mode
    modes = ("standby_replay", "standby_write", "standby_flush")
    mixed = [
        standby.background_sql_oneshot(
            f"WAIT FOR LSN '{mixed_target}' WITH (MODE '{modes[i % 3]}', timeout '1d')"
        )
        for i in range(6)
    ]

    # Verify all waiters are blocked
    standby.poll_query_until(
        "SELECT count(*) = 6 FROM pg_stat_activity WHERE wait_event LIKE 'WaitForWal%'"
    )

    # Resume replay (waiters should still be blocked as no WAL has arrived)
    standby.sql("SELECT pg_wal_replay_resume()")

    standby.poll_query_until("SELECT NOT pg_is_wal_replay_paused()")

    # Restore walreceiver to allow WAL to arrive
    resume_walreceiver(standby, saved)

    # Wait for all sessions to complete and close them
    for fut in mixed:
        fut.result()

    # Verify all modes reached the target LSN
    assert standby.sql(
        "SELECT pg_lsn_cmp((SELECT written_lsn FROM pg_stat_wal_receiver), $1) >= 0 "
        "AND pg_lsn_cmp(pg_last_wal_receive_lsn(), $1) >= 0 "
        "AND pg_lsn_cmp(pg_last_wal_replay_lsn(), $1) >= 0",
        mixed_target,
    ), "mixed mode waiters: all modes completed and reached target LSN"

    # 7e. Check the scenario of multiple primary_flush waiters on primary.
    # We start 5 background sessions waiting for different LSNs with primary_flush
    # mode.  Each waiter logs when done.
    pf_lsns = [insert_lsn(str(400 + i)) for i in range(5)]
    pf_waiters = [
        primary.background_sql_oneshot(
            f"WAIT FOR LSN '{pf_lsns[i]}' WITH (MODE 'primary_flush', timeout '1d')"
        )
        for i in range(5)
    ]
    for fut in pf_waiters:
        fut.result()
    assert (
        primary.sql("SELECT pg_lsn_cmp(pg_current_wal_flush_lsn(), $1)", pf_lsns[4])
        >= 0
    ), "multiple primary_flush waiters: primary flushed WAL up to target LSN"

    # 8. Check that the standby promotion terminates all standby wait modes.  Start
    # waiting for unreachable LSNs with standby_replay, standby_write, and
    # standby_flush modes, then promote.  Check the log for the relevant error
    # messages.  Also, check that waiting for already replayed LSN doesn't cause
    # an error even after promotion.
    lsn4 = primary.sql("SELECT pg_current_wal_insert_lsn() + 10000000000")
    lsn5 = primary.sql("SELECT pg_current_wal_insert_lsn()")

    # Start background sessions waiting for unreachable LSN with all modes
    wait_modes = ("standby_replay", "standby_write", "standby_flush")
    promote_waiters = [
        standby.background_sql_oneshot(f"WAIT FOR LSN '{lsn4}' WITH (MODE '{mode}')")
        for mode in wait_modes
    ]

    # Ensure all three waiters have registered before promoting.
    standby.poll_query_until(
        "SELECT count(*) = 3 FROM pg_stat_activity WHERE wait_event LIKE 'WaitForWal%'"
    )
    # Make sure standby will be promoted at least at the primary insert LSN we
    # have just observed.  Use pg_switch_wal() to force the insert LSN to be
    # written to WAL.
    primary.sql("SELECT pg_switch_wal()")

    primary.wait_for_catchup(standby)

    # Wait for all three sessions to get the error (each mode has distinct
    # message)
    with standby.log_contains(
        [
            f"Recovery ended before target LSN.*{word}"
            for word in ("was written", "was flushed", "was replayed")
        ],
        wait=True,
    ):
        standby.promote()
    # The client sessions are abandoned (their errors are irrelevant).
    for fut in promote_waiters:
        try:
            fut.result()
        except Exception:
            pass
    # Waiting for an already-replayed LSN exits immediately even after promotion.
    standby.sql(f"WAIT FOR LSN '{lsn5}'")

    assert (
        standby.sql(f"WAIT FOR LSN '{lsn4}' WITH (timeout '10ms', no_throw)")
        == "not in recovery"
    ), "WAIT FOR returns correct status after standby promotion"
    standby.stop()
    primary.stop()


def test_wait_for_lsn_archive_only_standby(create_pg):
    """standby_write and standby_flush on a standby with no walreceiver.

    Exercises the replay-position floor in GetCurrentLSNForWaitType(): with WAL
    arriving only by archive recovery, the walreceiver-tracked positions stay at
    their zero-initialised values."""
    # 9. Archive-only standby tests: verify standby_write/standby_flush work
    # without a walreceiver.  These exercises the replay-position floor in
    # GetCurrentLSNForWaitType().
    #
    # We set up a separate primary with archiving and an archive-only standby
    # (has_restoring, no has_streaming), so no walreceiver ever starts and the
    # shared walreceiver positions (writtenUpto, flushedUpto) stay at their
    # zero-initialized values.
    arc_primary = create_pg("arc_primary", allows_streaming=True, archiving=True)
    arc_primary.sql("CREATE TABLE arc_test AS SELECT generate_series(1,10) AS a")
    arc_backup = arc_primary.backup("arc_backup")
    # Generate WAL that will be archived and replayed on the standby.
    arc_primary.sql("INSERT INTO arc_test VALUES (generate_series(11, 20))")
    arc_target = arc_primary.sql("SELECT pg_current_wal_insert_lsn()")

    def archive_switch(node):
        """Force WAL to be archived by switching segments, then wait for
        archiving.
        """
        segment = node.sql("SELECT pg_walfile_name(pg_current_wal_lsn())")
        node.sql("SELECT pg_switch_wal()")

        node.poll_query_until(
            "SELECT last_archived_wal >= $1 FROM pg_stat_archiver", segment
        )

    archive_switch(arc_primary)

    # Create an archive-only standby: has_restoring but NOT has_streaming. No
    # primary_conninfo means no walreceiver ever starts.
    arc_standby = create_pg(
        "arc_standby", from_backup=arc_backup, restoring=arc_primary
    )

    # Wait for the standby to replay past our target LSN via archive recovery.
    arc_standby.poll_query_until(
        "SELECT pg_wal_lsn_diff(pg_last_wal_replay_lsn(), $1) >= 0", arc_target
    )

    # Sanity: verify no walreceiver is running.
    assert arc_standby.sql("SELECT count(*) FROM pg_stat_wal_receiver") == 0, (
        "arc_standby has no walreceiver"
    )

    # 9a. Getter fallback: standby_write/standby_flush succeed immediately when
    # the target LSN has already been replayed, even though writtenUpto and
    # flushedUpto are zero.  GetCurrentLSNForWaitType() returns
    # Max(walrcv_pos, replay), so replay >= target satisfies the check on the
    # first loop iteration without ever sleeping.
    for mode in ("standby_write", "standby_flush"):
        assert (
            arc_standby.sql(
                f"WAIT FOR LSN '{arc_target}' WITH (MODE '{mode}', timeout '3s', no_throw)"
            )
            == "success"
        ), f"{mode} succeeds on archive-only standby (getter fallback)"

    # 9b. Replay waker: standby_write/standby_flush waiters that go to sleep
    # (target > replay at entry) are woken when replay catches up.  This tests
    # that PerformWalRecovery() calls WaitLSNWakeup for STANDBY_WRITE and
    # STANDBY_FLUSH, not just STANDBY_REPLAY.
    #
    # Pause replay, archive more WAL, start background waiters, then resume
    # replay and verify the waiters complete.
    arc_standby.sql("SELECT pg_wal_replay_pause()")

    # Generate more WAL and archive it.
    arc_primary.sql("INSERT INTO arc_test VALUES (generate_series(21, 30))")
    arc_target2 = arc_primary.sql("SELECT pg_current_wal_insert_lsn()")
    archive_switch(arc_primary)

    # Start background waiters.  With replay paused, target > replay, so they
    # will sleep on WaitLatch.  They can only be woken by the startup process.
    arc_wf = arc_standby.background_sql_oneshot(
        f"WAIT FOR LSN '{arc_target2}' WITH (MODE 'standby_write', timeout '1d', no_throw)"
    )
    arc_ff = arc_standby.background_sql_oneshot(
        f"WAIT FOR LSN '{arc_target2}' WITH (MODE 'standby_flush', timeout '1d', no_throw)"
    )

    # Verify both waiters are blocked.
    arc_standby.poll_query_until(
        "SELECT count(*) = 2 FROM pg_stat_activity WHERE wait_event LIKE 'WaitForWal%'"
    )

    # Resume replay.  The startup process should wake the STANDBY_WRITE and
    # STANDBY_FLUSH waiters as it replays past their target.
    arc_standby.sql("SELECT pg_wal_replay_resume()")

    assert arc_wf.result() == "success", "standby_write waiter woken by replay"
    assert arc_ff.result() == "success", "standby_flush waiter woken by replay"

    arc_standby.stop()
    arc_primary.stop()


def test_wait_for_lsn_fresh_walreceiver(create_pg):
    """Walreceiver startup with fresh shared memory, and the fencepost.

    RequestXLogStreaming() seeds writtenUpto/flushedUpto from the
    segment-aligned receiveStart, which the replay floor then has to cover; the
    boundary checks probe the wait predicate at target == current +/- 1."""
    # 10. Fresh-shmem walreceiver startup (29e7dbf5e4d).
    # RequestXLogStreaming() initializes writtenUpto/flushedUpto to the
    # segment-aligned receiveStart only when receiveStart was invalid.
    # Restart the standby with the primary stopped, so the walreceiver cannot
    # connect and advance these values past the initial one before we observe it.
    # No background WAL during our probes.
    rcv_primary = create_pg(
        "rcv_primary", allows_streaming=True, conf={"autovacuum": False}
    )
    rcv_primary.sql("CREATE TABLE rcv_test AS SELECT generate_series(1,10) AS a")
    rcv_backup = rcv_primary.backup("rcv_backup")
    rcv_standby = create_pg(
        "rcv_standby", from_backup=rcv_backup, streaming_primary=rcv_primary
    )

    # Switch WAL segments mid-stream so the replay ends mid-segment after the
    # upcoming standby restart.  That guarantees replay sits ahead of the
    # segment-aligned position the walreceiver initializes to.
    rcv_primary.sql("INSERT INTO rcv_test VALUES (generate_series(11, 100))")
    rcv_primary.sql("SELECT pg_switch_wal()")
    rcv_primary.sql("INSERT INTO rcv_test VALUES (generate_series(101, 110))")

    rcv_primary.wait_for_catchup(rcv_standby)

    # Restart the standby with the primary down so the walreceiver can't update
    # writtenUpto/flushedUpto past the initial value.
    rcv_standby.stop()
    rcv_primary.stop()
    rcv_standby.start()

    rcv_standby.poll_query_until("SELECT pg_last_wal_receive_lsn() IS NOT NULL")

    # Freeze the replay so the (received, replay] window stays observable.
    rcv_standby.sql("SELECT pg_wal_replay_pause()")

    rcv_standby.poll_query_until("SELECT pg_get_wal_replay_pause_state() = 'paused'")

    rcv_receive = rcv_standby.sql("SELECT pg_last_wal_receive_lsn()")
    rcv_replay = rcv_standby.sql("SELECT pg_last_wal_replay_lsn()")

    assert rcv_standby.sql(
        "SELECT pg_wal_lsn_diff($1, $2) > 0", rcv_replay, rcv_receive
    ), "replay sits ahead of initial walreceiver flush position"

    assert (
        rcv_standby.sql(
            "SELECT mod(pg_wal_lsn_diff($1, '0/0'::pg_lsn), setting::numeric)::int "
            "FROM pg_settings WHERE name = 'wal_segment_size'",
            rcv_receive,
        )
        == 0
    ), "initial walreceiver flush position is segment-aligned"

    # WAIT FOR an rcv_replay LSN succeeds in standby_write / standby_flush modes
    # thanks to GetCurrentLSNForWaitType()'s replay floor.
    for mode in ("standby_write", "standby_flush"):
        assert (
            rcv_standby.sql(
                f"WAIT FOR LSN '{rcv_replay}' WITH (MODE '{mode}', timeout '5s', no_throw)"
            )
            == "success"
        ), f"{mode} succeeds for already-replayed LSN after standby restart"

    # Restore primary and resume replay so section 11 can reuse the clusters.
    # Generate fresh WAL after reconnecting so the walreceiver advances.
    rcv_standby.sql("SELECT pg_wal_replay_resume()")
    rcv_primary.start()
    rcv_primary.sql("INSERT INTO rcv_test VALUES (generate_series(111, 120))")

    rcv_primary.wait_for_catchup(rcv_standby)

    # 11. Off-by-one boundary checks for the wait predicate target <=
    # currentLSN.  Stop the walreceiver before pausing replay (stopping
    # after pause can hang -- see section 7d) so both replay and
    # walreceiver positions are frozen.
    saved = stop_walreceiver(rcv_standby)
    rcv_standby.sql("SELECT pg_wal_replay_pause()")

    rcv_standby.poll_query_until("SELECT pg_get_wal_replay_pause_state() = 'paused'")

    replay_lsn = rcv_standby.sql("SELECT pg_last_wal_replay_lsn()")
    # 11a. standby_replay exact fencepost.  The replay position is frozen, so this
    # probes the standby_replay predicate directly.
    _, replay_lsn_plus = check_fencepost(
        rcv_standby, "standby_replay", replay_lsn, "standby_replay"
    )

    flush_lsn = rcv_standby.sql("SELECT pg_last_wal_receive_lsn()")

    assert rcv_standby.sql(
        "SELECT pg_wal_lsn_diff($1, $2) >= 0", flush_lsn, replay_lsn
    ), "standby_flush boundary is not masked by replay floor"
    # 11b. standby_flush exact fencepost.  pg_last_wal_receive_lsn() exposes the
    # flushed walreceiver position even after walreceiver exits, so this probes
    # the standby_flush predicate directly.  standby_write has no stable
    # SQL-visible boundary once walreceiver is stopped; it is covered by the
    # replay-floor and waiter wakeup tests above.
    check_fencepost(rcv_standby, "standby_flush", flush_lsn, "standby_flush")

    # 11c. A sleeping waiter at current + 1 wakes once replay advances
    # past it.  Start the waiter while replay is still paused so it is
    # guaranteed to sleep at replay_lsn_plus regardless of whether
    # flush_lsn > replay_lsn.  Then resume replay and restart the
    # walreceiver to deliver new WAL.
    rcv_primary.sql("INSERT INTO rcv_test VALUES (generate_series(200, 210))")
    boundary_fut = rcv_standby.background_sql_oneshot(
        f"WAIT FOR LSN '{replay_lsn_plus}' WITH (MODE 'standby_replay', timeout '1d', no_throw)"
    )

    rcv_standby.poll_query_until(
        "SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'WaitForWalReplay'"
    )
    rcv_standby.sql("SELECT pg_wal_replay_resume()")
    resume_walreceiver(rcv_standby, saved)

    assert boundary_fut.result() == "success", (
        "standby_replay: waiter at current + 1 wakes when replay advances"
    )

    rcv_standby.stop()
    rcv_primary.stop()


@require_extension("test_wait_lsn")
def test_wait_for_lsn_stale_wakeup(create_pg):
    """A standby_write waiter removed from the waiters heap by a stale wakeup
    must re-register if its target has not actually been reached.

    This is section 11d of the Perl test, which runs it in a SKIP block on the
    clusters of section 10. Here it is a test of its own, with its own
    clusters, so that it shows up as skipped when test_wait_lsn is missing
    (e.g. under installcheck)."""
    # No background WAL during our probes.
    primary = create_pg(
        "stale_primary", allows_streaming=True, conf={"autovacuum": False}
    )
    primary.sql("CREATE TABLE rcv_test AS SELECT generate_series(1,10) AS a")
    primary.sql("CREATE EXTENSION test_wait_lsn")
    backup = primary.backup("stale_backup")
    standby = create_pg("stale_standby", from_backup=backup, streaming_primary=primary)
    primary.wait_for_catchup(standby)

    # Stop streaming before generating the target.  This keeps both the
    # walreceiver write position and the replay position below the target.
    saved = stop_walreceiver(standby)
    primary.sql("INSERT INTO rcv_test VALUES (300)")
    stale_target = primary.sql("SELECT pg_current_wal_insert_lsn()")

    # Keep WAIT FOR untimed.  After streaming resumes below, its own timeout
    # could wake the backend and make the completion check pass even if the
    # WAL-progress wakeup were lost.
    stale_session = standby.connect()
    stale_waiter_pid = stale_session.sql("SELECT pg_backend_pid()")
    stale_fut = stale_session.background_sql(
        f"WAIT FOR LSN '{stale_target}' WITH (MODE 'standby_write')"
    )

    registered = "SELECT test_wait_lsn_waiter_is_registered($1, 'standby_write', $2)"
    standby.poll_query_until(registered, stale_waiter_pid, stale_target)

    # The write position is below the target because the receiver was stopped
    # before the target was generated.  Verify that replay is below it too.
    assert standby.sql(
        "SELECT pg_wal_lsn_diff($1::pg_lsn, pg_last_wal_replay_lsn()) > 0",
        stale_target,
    ), "standby replay reached the target before the stale wakeup"

    # Simulate the state left by a wakeup that became stale before the waiter
    # could recheck its target.  The helper removes the waiter from the heap
    # and wakes it without changing the actual write or replay positions.  To
    # the waiter, this is indistinguishable from the position reaching the
    # target and then decreasing before the recheck.
    standby.sql("SELECT test_wait_lsn_wakeup('standby_write', $1)", stale_target)

    # The position is still below the target, so the waiter must restore its
    # heap registration before sleeping again.
    standby.poll_query_until(registered, stale_waiter_pid, stale_target)

    # Reach the target for real; WAL progress should wake the re-registered
    # waiter.
    resume_walreceiver(standby, saved)

    assert stale_fut.result() == "success", (
        "standby_write waiter completes once the target is reached"
    )
    stale_session.close()


def test_wait_for_lsn_cascade_timeline_switch(create_pg):
    """A waiter on a cascade standby survives its upstream's promotion.

    The cascade walreceiver has to reconnect on the new timeline and keep
    replaying across the boundary for the waiter to be satisfied."""
    # 12. Timeline switch on a cascade standby.  A WAIT FOR LSN waiter on
    # a cascade standby must survive its upstream's promotion: the
    # cascade walreceiver reconnects on the new timeline and replay
    # continues across the boundary.
    tl_primary = create_pg(
        "tl_primary", allows_streaming=True, conf={"autovacuum": False}
    )
    tl_primary.sql("CREATE TABLE tl_test AS SELECT generate_series(1, 10) AS a")
    tl_backup = tl_primary.backup("tl_backup")
    tl_standby1 = create_pg(
        "tl_standby1", from_backup=tl_backup, streaming_primary=tl_primary
    )
    # standby2 cascades from standby1.
    tl_backup2 = tl_standby1.backup("tl_backup2")
    tl_standby2 = create_pg(
        "tl_standby2", from_backup=tl_backup2, streaming_primary=tl_standby1
    )

    tl_primary.sql("INSERT INTO tl_test VALUES (generate_series(11, 20))")
    lsn = tl_primary.lsn("flush")

    tl_primary.wait_for_catchup(tl_standby1, "replay", lsn)
    tl_standby1.wait_for_catchup(tl_standby2, "replay", lsn)

    # Target LSN well past current insert LSN, so reaching it requires WAL
    # produced on the new timeline.  Pause replay so the waiter is guaranteed to
    # sleep across the promotion.
    tl_target = tl_primary.sql("SELECT (pg_current_wal_insert_lsn() + 65536)::text")
    tl_standby2.sql("SELECT pg_wal_replay_pause()")

    tl_standby2.poll_query_until("SELECT pg_get_wal_replay_pause_state() = 'paused'")

    tl_fut = tl_standby2.background_sql_oneshot(
        f"WAIT FOR LSN '{tl_target}' WITH (MODE 'standby_replay', timeout '1d', no_throw)"
    )

    tl_standby2.poll_query_until(
        "SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'WaitForWalReplay'"
    )

    # Promote standby1 to TLI 2; produce enough WAL on the new timeline to push
    # past tl_target and force a segment switch.
    tl_standby1.promote()
    tl_standby1.sql("INSERT INTO tl_test VALUES (generate_series(21, 1020))")
    tl_standby1.sql("SELECT pg_switch_wal()")
    tl_standby2.sql("SELECT pg_wal_replay_resume()")

    tl_standby2.poll_query_until("SELECT received_tli > 1 FROM pg_stat_wal_receiver")

    assert tl_fut.result() == "success", (
        "WAIT FOR LSN survives upstream promotion and timeline switch on cascade standby"
    )


@require_injection_points()
@require_extension("test_wait_lsn")
def test_wait_for_lsn_implicit_flush_wakeup(create_pg):
    """An implicit flush at a segment boundary wakes primary_flush waiters.

    AdvanceXLInsertBuffer() asks XLogWrite() only to write WAL, but completing
    a segment also advances the flush position."""
    # 13. AdvanceXLInsertBuffer() asks XLogWrite() only to write WAL, but
    # completing a segment also advances the flush position.  Verify that this
    # implicit flush wakes primary_flush waiters.
    node = create_pg(
        "flush_wake_primary",
        initdb_opts=["--wal-segsize=1"],
        conf={
            "wal_buffers": 4,
            "autovacuum": False,
            "checkpoint_timeout": "1h",
            "max_wal_size": "1GB",
            "bgwriter_lru_maxpages": 0,
        },
    )

    assert (
        node.sql("SELECT setting::int FROM pg_settings WHERE name = 'wal_segment_size'")
        == 1024 * 1024
    ), "dedicated node runs with 1MB WAL segments"

    # Keep the WAL writer out of XLogBackgroundFlush(), whose ordinary flush
    # wakeup would mask the path under test.
    node.sql_batch(
        "CREATE EXTENSION injection_points",
        "CREATE EXTENSION test_wait_lsn",
        "SELECT injection_points_attach('walwriter-before-background-flush', 'wait')",
    )
    node.wait_for_event("walwriter", "walwriter-before-background-flush")

    writer = node.connect()

    # Begin in a fresh segment, then round the insert pointer up to its end.
    writer.sql("SELECT pg_switch_wal()")
    target_lsn = writer.sql(
        """
        WITH p AS
        (
            SELECT pg_current_wal_insert_lsn() AS lsn,
                   setting::numeric AS segsz
            FROM pg_settings
            WHERE name = 'wal_segment_size'
        )
        SELECT lsn + (segsz - (lsn - '0/0'::pg_lsn) % segsz)
        FROM p
        """
    )

    assert writer.sql("SELECT pg_current_wal_flush_lsn() < $1::pg_lsn", target_lsn), (
        "wakeup target is not flushed initially"
    )

    waiter = node.connect()
    waiter_pid = waiter.sql("SELECT pg_backend_pid()")

    node.sql("SELECT injection_points_attach('wait-for-lsn-after-register', 'wait')")

    wait_fut = waiter.background_sql(
        f"WAIT FOR LSN '{target_lsn}' WITH (MODE 'primary_flush', no_throw)"
    )
    node.wait_for_event("client backend", "wait-for-lsn-after-register")

    registered = "SELECT test_wait_lsn_waiter_is_registered($1, 'primary_flush', $2)"
    assert node.sql(registered, waiter_pid, target_lsn), (
        "primary_flush waiter is registered before the implicit flush"
    )

    # A 2MB nontransactional, non-flushing logical message assigns no XID and
    # dirties no relation buffers.  With four WAL buffers, it must recycle through
    # the target.  Keep its transaction open so implicit transaction completion
    # cannot call XLogSetAsyncXactLSN(); the xid-less rollback writes no WAL.
    writer.sql("BEGIN")
    writer.sql(
        "SELECT pg_logical_emit_message("
        "false, 'wfl', repeat('x', 2 * 1024 * 1024), false)"
    )

    assert writer.sql("SELECT pg_current_wal_flush_lsn() >= $1::pg_lsn", target_lsn), (
        "finishing a segment advanced flush through the target"
    )

    # The waiter is still parked, so only a progress waker can remove it.
    assert not node.sql(registered, waiter_pid, target_lsn), (
        "implicit segment flush removes the primary_flush waiter"
    )

    node.sql_batch(
        "SELECT injection_points_detach('wait-for-lsn-after-register')",
        "SELECT injection_points_wakeup('wait-for-lsn-after-register')",
    )

    assert wait_fut.result() == "success", (
        "primary_flush waiter returns success after release"
    )

    # Detach first so that the WAL writer cannot immediately re-enter the point.
    node.sql_batch(
        "SELECT injection_points_detach('walwriter-before-background-flush')",
        "SELECT injection_points_wakeup('walwriter-before-background-flush')",
    )

    writer.sql("ROLLBACK")
