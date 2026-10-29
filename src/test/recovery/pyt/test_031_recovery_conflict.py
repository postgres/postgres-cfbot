# Copyright (c) 2021-2026, PostgreSQL Global Development Group

"""Port of src/test/recovery/t/031_recovery_conflict.pl.

Test that connections to a hot standby are correctly canceled when a recovery
conflict is detected, and that pg_stat_database_conflicts is populated. Each
conflict type kills the standby session that triggers it, so a fresh background
session is opened per scenario (the Perl test reconnects one psql).
"""

import pytest
from libpq import LibpqError
from pypg import pg_test_timeout_default

TABLESPACE1 = "test_recovery_conflict_tblspc"
TEST_DB = "test_db"
TABLE1 = "test_recovery_conflict_table1"
TABLE2 = "test_recovery_conflict_table2"
CURSOR1 = "test_recovery_conflict_cursor"


def test_recovery_conflict(create_pg):
    primary = create_pg(
        "primary",
        allows_streaming=True,
        conf={
            "allow_in_place_tablespaces": True,
            "log_temp_files": 0,
            # The test must control when pruning WAL is generated.
            "autovacuum": False,
            # for deadlock test
            "max_prepared_transactions": 10,
            # wait some to test the wait paths as well, but not long
            "max_standby_streaming_delay": "50ms",
            "temp_tablespaces": TABLESPACE1,
            # Some recovery-conflict logging is only exercised after
            # deadlock_timeout; give minimal coverage of that code.
            "log_recovery_conflict_waits": True,
            "deadlock_timeout": "10ms",
        },
    )
    primary.sql(f"CREATE TABLESPACE {TABLESPACE1} LOCATION ''")

    backup = primary.backup("my_backup")
    standby = create_pg("standby", from_backup=backup, streaming_primary=primary)

    # Use a new database to be able to trigger a database recovery conflict.
    # Nearly everything below runs against it on both nodes, so make it the
    # default rather than passing dbname= to every call. The final scenario
    # drops it, and reaches back to postgres explicitly to do so.
    primary.sql(f"CREATE DATABASE {TEST_DB}")
    primary.default_connection_options = {"dbname": TEST_DB}
    standby.default_connection_options = {"dbname": TEST_DB}

    primary.sql_batch_oneshot(
        f"CREATE TABLE {TABLE1}(a int, b int)",
        f"INSERT INTO {TABLE1} SELECT i % 3, 0 FROM generate_series(1,20) i",
        f"CREATE TABLE {TABLE2}(a int, b int)",
    )

    primary.wait_for_catchup(standby)

    def check_conflict_stat(conflict_type):
        # Poll rather than read once: the startup process flushes recovery
        # conflict stats to shared memory with a small delay. Check for at
        # least one rather than exactly one, since a backend can process the
        # same recovery conflict more than once before it exits.
        # conflict_type names a column, so it has to be interpolated; the
        # database name is a value and is bound.
        standby.poll_query_until(
            f"SELECT confl_{conflict_type} > 0 FROM pg_stat_database_conflicts "
            "WHERE datname = $1",
            TEST_DB,
        )

    def conflicting_session(*statements, expect, what):
        """Open a standby session and put it in the state that will conflict.

        ``statements`` run in a transaction; the last one's result must equal
        ``expect``, which is how we know the cursor, lock or temp file the
        conflict needs is really held before the primary does its thing.
        """
        session = standby.connect()
        assert session.sql_batch("BEGIN", *statements)[-1] == expect, (
            f"{what} established"
        )
        return session

    expected_conflicts = 0

    ## RECOVERY CONFLICT 1: Buffer pin conflict
    expected_conflicts += 1

    # Aborted INSERT on primary that will be cleaned up by vacuum. Has to be old
    # enough so that there's not a snapshot conflict before the buffer pin
    # conflict.
    #
    # The statements run on one held connection so the explicit transactions
    # behave as in psql (a single multi-statement PQexec would merge them into
    # one implicit transaction).
    with primary.connect() as c:
        c.sql("BEGIN")
        c.sql(f"INSERT INTO {TABLE1} VALUES (1,0)")
        c.sql("ROLLBACK")

        # ensure flush, rollback doesn't do so
        c.sql("BEGIN")
        c.sql(f"LOCK {TABLE1}")
        c.sql("COMMIT")
    primary.wait_for_catchup(standby)

    # DECLARE and use a cursor on standby, causing buffer with the only block of
    # the relation to be pinned on the standby. FETCH FORWARD should return a 0
    # since all values of b in the table are 0.
    bg = conflicting_session(
        f"DECLARE {CURSOR1} CURSOR FOR SELECT b FROM {TABLE1}",
        f"FETCH FORWARD FROM {CURSOR1}",
        expect=0,
        what="buffer pin conflict: cursor with conflicting pin",
    )

    with standby.log_contains(
        "User was holding shared buffer pin for too long", wait=True
    ):
        # VACUUM FREEZE on the primary
        primary.sql_oneshot(f"VACUUM FREEZE {TABLE1}")

        # Wait for catchup. Existing connection will be terminated before
        # replay is finished, so waiting for catchup ensures that there is no
        # race between encountering the recovery conflict which causes the
        # disconnect and checking the logfile for the terminated connection.
        primary.wait_for_catchup(standby)
    bg.close()
    check_conflict_stat("bufferpin")

    ## RECOVERY CONFLICT 2: Snapshot conflict
    expected_conflicts += 1
    primary.sql_oneshot(
        f"INSERT INTO {TABLE1} SELECT i, 0 FROM generate_series(1,20) i"
    )

    primary.wait_for_catchup(standby)

    # DECLARE and FETCH from cursor on the standby
    bg = conflicting_session(
        f"DECLARE {CURSOR1} CURSOR FOR SELECT b FROM {TABLE1}",
        f"FETCH FORWARD FROM {CURSOR1}",
        expect=0,
        what="snapshot conflict: cursor with conflicting snapshot",
    )

    # Do some HOT updates
    primary.sql_oneshot(f"UPDATE {TABLE1} SET a = a + 1 WHERE a > 2")

    with standby.log_contains(
        "User query might have needed to see row versions that must be removed",
        wait=True,
    ):
        # VACUUM FREEZE, pruning those dead tuples
        primary.sql_oneshot(f"VACUUM FREEZE {TABLE1}")

        # Wait for attempted replay of PRUNE records
        primary.wait_for_catchup(standby)
    bg.close()
    check_conflict_stat("snapshot")

    ## RECOVERY CONFLICT 3: Lock conflict
    expected_conflicts += 1

    # acquire lock to conflict with
    bg = conflicting_session(
        f"LOCK TABLE {TABLE1} IN ACCESS SHARE MODE",
        "SELECT 1",
        expect=1,
        what="lock conflict: conflicting lock",
    )

    with standby.log_contains(
        "User was holding a relation lock for too long", wait=True
    ):
        # DROP TABLE containing block which standby has in a pinned buffer
        primary.sql_oneshot(f"DROP TABLE {TABLE1}")
        primary.wait_for_catchup(standby)
    bg.close()
    check_conflict_stat("lock")

    ## RECOVERY CONFLICT 4: Tablespace conflict
    expected_conflicts += 1

    # DECLARE a cursor for a query which, with sufficiently low work_mem, will
    # spill tuples into temp files in the temporary tablespace created during
    # setup.
    bg = conflicting_session(
        "SET work_mem = '64kB'",
        f"DECLARE {CURSOR1} CURSOR FOR SELECT count(*) FROM generate_series(1,6000)",
        f"FETCH FORWARD FROM {CURSOR1}",
        expect=6000,
        what="tablespace conflict: cursor with conflicting temp file",
    )

    with standby.log_contains(
        "User was or might have been using tablespace that must be dropped",
        wait=True,
    ):
        # Drop the tablespace currently containing spill files for the query
        # on the standby
        primary.sql_oneshot(f"DROP TABLESPACE {TABLESPACE1}")
        primary.wait_for_catchup(standby)
    bg.close()
    check_conflict_stat("tablespace")

    ## RECOVERY CONFLICT 5: Deadlock
    expected_conflicts += 1

    # Want to test recovery deadlock conflicts, not buffer pin conflicts. Without
    # changing max_standby_streaming_delay it'd be timing dependent what we hit
    # first
    standby.append_conf(max_standby_streaming_delay=f"{pg_test_timeout_default()}s")
    standby.pg_ctl("restart")

    # Generate a few dead rows, to later be cleaned up by vacuum. Then acquire a
    # lock on another relation in a prepared xact, so it's held continuously by
    # the startup process. The standby psql will block acquiring that lock while
    # holding a pin that vacuum needs, triggering the deadlock.
    with primary.connect() as setup:
        setup.sql(f"CREATE TABLE {TABLE1}(a int, b int)")
        setup.sql(f"INSERT INTO {TABLE1} VALUES (1)")
        with primary.connect() as c:
            c.sql("BEGIN")
            c.sql(f"INSERT INTO {TABLE1}(a) SELECT generate_series(1, 100) i")
            c.sql("ROLLBACK")

            # The prepared transaction holds the lock on TABLE2 continuously,
            # independently of this session.
            c.sql_batch("BEGIN", f"LOCK TABLE {TABLE2}", "PREPARE TRANSACTION 'lock'")
        setup.sql(f"INSERT INTO {TABLE1}(a) VALUES (170)")
        setup.sql("SELECT txid_current()")
    primary.wait_for_catchup(standby)

    bg = conflicting_session(
        f"DECLARE {CURSOR1} CURSOR FOR SELECT a FROM {TABLE1}",
        f"FETCH FORWARD FROM {CURSOR1}",
        expect=1,
        what="deadlock: cursor holding the pin vacuum needs",
    )

    # wait for lock held by the prepared transaction (blocks)
    waiter = bg.background_sql(f"SELECT * FROM {TABLE2}")

    try:
        # just to make sure we're waiting for lock already
        standby.poll_query_until(
            "SELECT 'waiting' FROM pg_locks WHERE locktype = 'relation' AND NOT granted",
            expected="waiting",
        )

        # VACUUM FREEZE will prune away rows, causing a buffer pin conflict, while
        # standby psql is waiting on lock
        with standby.log_contains(
            "User transaction caused buffer deadlock with recovery.", wait=True
        ):
            primary.sql_oneshot(f"VACUUM FREEZE {TABLE1}")
            primary.wait_for_catchup(standby)
    finally:
        # Unlike the other conflicts this one resolves by canceling the
        # statement (ERROR), not terminating the connection, so the session
        # survives with an aborted transaction.
        with pytest.raises(
            LibpqError, match="canceling statement due to conflict with recovery"
        ):
            waiter.result()
    # Disconnect so the backend exits and flushes its pending conflict stat to
    # shared memory (an idle surviving backend would not flush it in time).
    bg.close()
    check_conflict_stat("deadlock")

    # Clean up for the next tests.
    primary.sql_oneshot("ROLLBACK PREPARED 'lock'")
    standby.append_conf(max_standby_streaming_delay="50ms")
    standby.pg_ctl("restart")

    # Check that at least the expected number of conflicts show in
    # pg_stat_database, before the database is dropped. A backend can process
    # the same recovery conflict more than once before it exits.
    assert (
        standby.sql_oneshot(
            "SELECT conflicts FROM pg_stat_database WHERE datname = $1", TEST_DB
        )
        >= expected_conflicts
    ), f"at least {expected_conflicts} recovery conflicts shown in pg_stat_database"

    # RECOVERY CONFLICT 6: Database conflict. A live standby connection to the
    # database is terminated when the drop is replayed.
    db_conn = standby.connect()
    db_conn.sql("SELECT 1")

    with standby.log_contains(
        "User was connected to a database that must be dropped", wait=True
    ):
        # The primary is done with the database, and has to be: DROP DATABASE
        # refuses while anything is still connected, including the primary's
        # own cached sql() connection. Switching the default back closes it.
        # The standby's connection stays -- being terminated by the drop is
        # the point.
        primary.default_connection_options = {}
        primary.sql(f"DROP DATABASE {TEST_DB}")
        primary.wait_for_catchup(standby)
