# Copyright (c) 2025-2026, PostgreSQL Global Development Group

"""Port of src/test/modules/test_aio/t/001_aio.pl.

Exercises the AIO subsystem through the test_aio extension, once per supported
io_method. Where the Perl test matches psql stderr (WARNING/ERROR text) with
`psql_like`, this uses captured server messages (``pytest.warns`` /
``PostgresWarning``/``PostgresNotice``) and ``pytest.raises`` instead, and
asserts on real result values rather than psql's text output.
"""

import contextlib
import re
import warnings

import pytest
from libpq import LibpqError, PostgresMessage, PostgresWarning
from pypg import skip_unless_injection_points, wait_until
from pypg.bins import postgres

IO_METHODS = ["worker", "io_uring", "sync"]

CONFIGURE = {
    "shared_preload_libraries": "test_aio",
    "log_min_messages": "DEBUG3",
    "log_statement": "all",
    "log_error_verbosity": "default",
    "temp_buffers": 100,
}


def supported_io_methods():
    # Probe the valid io_method values from the error message for an invalid
    # one. -C avoids the superuser check (needed when running as administrator
    # on Windows).
    r = postgres.check_all("-C", "invalid", "-c", "io_method=invalid", exit_code=1)
    m = re.search(r"Available values: ([^.]+)\.", r.stderr)
    assert m, f"can't determine supported io_method values: {r.stderr}"
    return m.group(1)


@pytest.fixture(scope="module", params=IO_METHODS)
def node(request, create_pg_module):
    # One configured node per io_method, shared across this module's sub-tests
    # (mirrors the Perl test_io_method, which reuses a single node per method).
    method = request.param
    if method not in supported_io_methods():
        pytest.skip(f"io_method {method} not supported by this build")

    conf = {**CONFIGURE, "io_method": method}
    if method == "sync":
        conf["io_max_concurrency"] = 4
    node = create_pg_module(f"aio_{method}", conf=conf)

    assert node.sql("SHOW io_method") == method, "io_method set correctly"

    node.sql("CREATE EXTENSION test_aio")
    node.sql(
        "CREATE TABLE tbl_corr(data int not null) WITH (AUTOVACUUM_ENABLED = false)"
    )
    node.sql("CREATE TABLE tbl_ok(data int not null) WITH (AUTOVACUUM_ENABLED = false)")
    node.sql("INSERT INTO tbl_corr SELECT generate_series(1, 10000)")
    node.sql("INSERT INTO tbl_ok SELECT generate_series(1, 10000)")
    node.sql("SELECT grow_rel('tbl_corr', 16)")
    node.sql("SELECT grow_rel('tbl_ok', 16)")
    node.sql("SELECT modify_rel_block('tbl_corr', 1, corrupt_header=>true)")
    node.sql("CHECKPOINT")
    return node


def wait_block(node, bg, sql, wait_event, params=(), simplify_result=True):
    """Dispatch ``sql`` on background session ``bg``, wait until bg's own
    backend parks on ``wait_event``, and return the Future.

    ``params`` are bound to ``sql``'s placeholders. ``simplify_result`` is
    passed to background_sql: pass False when the result is a row set, so it
    arrives as a list of tuples however many rows it has.
    """
    pid = bg.sql("SELECT pg_backend_pid()")
    fut = bg.background_sql(sql, *params, simplify_result=simplify_result)
    node.poll_query_until(
        "SELECT wait_event FROM pg_stat_activity WHERE pid = $1",
        pid,
        expected=wait_event,
    )
    return fut


def wait_block_any_backend(node, bg, sql, wait_event, params=(), simplify_result=True):
    """Like ``wait_block``, but wait for *any* backend to reach ``wait_event``.

    For waits that happen somewhere other than the session that issued the
    query: the completion_wait injection point below fires in whichever process
    runs the IO, which under io_method=worker is an IO worker rather than bg.
    """
    fut = bg.background_sql(sql, *params, simplify_result=simplify_result)
    node.poll_query_until(
        "SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = $1",
        wait_event,
        expected=True,
    )
    return fut


@contextlib.contextmanager
def no_messages():
    """Assert the server sends no NOTICE/WARNING during the block.

    Uses recording (not a ``"error"`` warning filter): a warning raised inside
    libpq's notice callback would be swallowed by ctypes rather than
    propagated, so we collect and check afterwards instead.
    """
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        yield
    msgs = [str(w.message) for w in caught if issubclass(w.category, PostgresMessage)]
    assert not msgs, f"unexpected server message(s): {msgs}"


def test_handle(node):
    """Sanity checks for the IO handle API."""

    def leaks():
        return pytest.warns(PostgresWarning, match="leaked AIO handle")

    # The leak warning is reported when the (sub)transaction ends, so the
    # explicit cases run their COMMIT/ROLLBACK inside the pytest.warns block.

    # leak warning: implicit xact
    with leaks():
        node.sql("SELECT handle_get()")

    # leak warning: explicit xact
    with leaks():
        node.sql("BEGIN")
        node.sql("SELECT handle_get()")
        node.sql("COMMIT")

    # leak warning: explicit xact, rollback
    with leaks():
        node.sql("BEGIN")
        node.sql("SELECT handle_get()")
        node.sql("ROLLBACK")

    # leak warning: subtrans
    with leaks():
        node.sql("BEGIN")
        node.sql("SAVEPOINT foo")
        node.sql("SELECT handle_get()")
        node.sql("COMMIT")

    # leak warning + error: released in different command (thus resowner)
    #
    # The COMMIT both cleans up the aborted transaction and is where the leaked
    # handle is reported.
    with leaks():
        node.sql("BEGIN")
        node.sql("SELECT handle_get()")
        with pytest.raises(LibpqError, match="release in unexpected state"):
            node.sql("SELECT handle_release_last()")
        node.sql("COMMIT")

    # no leak, release in same command
    with no_messages():
        node.sql("BEGIN")
        node.sql("SELECT handle_get() UNION ALL SELECT handle_release_last()")
        node.sql("COMMIT")

        # normal handle use
        node.sql("SELECT handle_get_release()")

    # should error out, API violation
    with pytest.raises(
        LibpqError, match="API violation: Only one IO can be handed out"
    ):
        node.sql("SELECT handle_get_twice()")

    # recover after error in implicit xact
    with pytest.raises(LibpqError, match="as you command"):
        node.sql("SELECT handle_get_and_error()")
    node.sql("SELECT handle_get_release()")

    # recover after error in explicit xact
    node.sql("BEGIN")
    with pytest.raises(LibpqError, match="as you command"):
        node.sql("SELECT handle_get_and_error()")
    node.sql("ROLLBACK")
    node.sql("SELECT handle_get_release()")

    # recover after error in subtrans
    node.sql("BEGIN")
    node.sql("SAVEPOINT foo")
    with pytest.raises(LibpqError, match="as you command"):
        node.sql("SELECT handle_get_and_error()")
    node.sql("ROLLBACK TO SAVEPOINT foo")
    node.sql("SELECT handle_get_release()")
    node.sql("ROLLBACK")


def test_batchmode(node):
    """Sanity checks for the batchmode API."""

    # In a build with RELCACHE_FORCE_RELEASE and CATCACHE_FORCE_RELEASE, just
    # using SELECT batch_start() causes spurious test failures, because the
    # lookup of the type information when printing the result tuple also
    # starts a batch. The easiest way around is to not print a result tuple.
    batch_start = "SELECT WHERE batch_start() IS NULL"

    def open_batch():
        return pytest.warns(PostgresWarning, match="open AIO batch at end")

    # leak warning & recovery: implicit xact
    with open_batch():
        node.sql(batch_start)

    # leak warning & recovery: explicit xact
    with open_batch():
        node.sql("BEGIN")
        node.sql(batch_start)
        node.sql("COMMIT")

    # leak warning & recovery: explicit xact, rollback
    #
    # XXX: This doesn't fail right now, due to not getting a chance to do
    # something at transaction command commit. That's not a correctness issue,
    # it just means it's a bit harder to find buggy code, so the rollback case
    # is not asserted here either.

    # no warning, batch closed in same command
    with no_messages():
        node.sql(f"{batch_start} UNION ALL SELECT WHERE batch_end() IS NULL")


@pytest.mark.parametrize("persistency", ["normal", "temporary"])
def test_io_error(node, persistency):
    """Simple cases of invalid pages are reported."""

    if persistency == "normal":
        tbl = "tbl_corr"
        page_re = r'invalid page in block 1 of relation "base/\d+/\d+'
    else:
        tbl = "tmp_corr"
        # A temporary relation's file path carries a t<backend>_ prefix.
        page_re = r'invalid page in block 1 of relation "base/\d+/t\d+_\d+'

        # It also has to be corrupted and read in the same session, so unlike
        # in the "normal" case it cannot come from the module fixture.
        node.sql(f"CREATE TEMPORARY TABLE {tbl}(data int not null)")
        node.sql(f"INSERT INTO {tbl} SELECT generate_series(1, 10000)")
        node.sql("SELECT modify_rel_block($1, 1, corrupt_header=>true)", tbl)

    # verify the error is reported in custom C code
    with pytest.raises(LibpqError, match=page_re):
        node.sql("SELECT read_rel_block_ll($1, 1)", tbl)
    # verify the error is reported for bufmgr reads, seq scan
    with pytest.raises(LibpqError, match=page_re):
        node.sql(f"SELECT count(*) FROM {tbl}")
    # verify the error is reported for bufmgr reads, tid scan
    with pytest.raises(LibpqError, match=page_re):
        node.sql(f"SELECT count(*) FROM {tbl} WHERE ctid = '(1, 1)'")


def test_complete_foreign(node):
    """A read started but not awaited by one backend can be completed by
    another (or after the starting backend exits)."""
    a = node.connect()
    b = node.connect()

    # Test that if the backend issuing a read doesn't wait for the IO's
    # completion, another backend can complete the IO
    a.sql("SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>false)")

    # Check that another backend can read the relevant block
    with no_messages():
        assert b.sql("SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1") == 1, (
            "another backend can read the relevant block"
        )

    # Test that if the backend issuing a read exits before the IO completes,
    # another backend can still complete the IO
    a.sql("SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>false)")
    a.close()
    a = node.connect()
    with no_messages():
        assert b.sql("SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1") == 1, (
            "another backend completes the IO after the issuing backend exited"
        )

    # Read a tbl_corr block, then sleep. The other session will retry the IO
    # and also fail. The easiest thing to verify that seems to be to check
    # that both are in the log.
    with node.log_contains(
        [r"LOG[^\n]+invalid page in", r"ERROR[^\n]+invalid page in"], wait=True
    ):
        a.sql("SELECT read_rel_block_ll('tbl_corr', 1, wait_complete=>false)")
        with pytest.raises(LibpqError, match="invalid page in block"):
            b.sql("SELECT count(*) FROM tbl_corr WHERE ctid = '(1,1)' LIMIT 1")


def test_close_fd(node):
    """FDs being closed while IO is in progress is handled."""

    with no_messages():
        node.sql(
            "SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>true, "
            "batchmode_enter=>true, smgrreleaseall=>true, batchmode_exit=>true)"
        )
        node.sql(
            "SELECT read_rel_block_ll('tbl_ok', 1, wait_complete=>false, "
            "batchmode_enter=>true, smgrreleaseall=>true, batchmode_exit=>true)"
        )
        assert (
            node.sql("SELECT count(*) FROM tbl_ok WHERE ctid = '(1,1)' LIMIT 1") == 1
        ), "reads work after smgrreleaseall() in batchmode"


@pytest.mark.parametrize("persistency", ["normal", "unlogged", "temporary"])
def test_invalidate(node, persistency):
    """A relation removed (rollback or DROP) while IO is ongoing is handled."""

    kind = "" if persistency == "normal" else persistency
    tbl = f"{persistency}_transactional"
    create = (
        f"CREATE {kind} TABLE {tbl} (id int not null, data text not null) "
        "WITH (AUTOVACUUM_ENABLED = false)"
    )
    insert = (
        f"INSERT INTO {tbl}(id, data) "
        "SELECT generate_series(1, 10000) as id, repeat('a', 200)"
    )

    # Verify that outstanding read IO does not cause problems with
    # AbortTransaction -> smgrDoPendingDeletes -> smgrdounlinkall -> ...
    # -> Invalidate[Local]Buffer.
    node.sql("BEGIN")
    node.sql(create)
    node.sql(insert)
    node.sql("SELECT read_rel_block_ll($1, 1, wait_complete=>false)", tbl)
    with no_messages():
        node.sql("ROLLBACK")

    # Verify that outstanding read IO does not cause problems with
    # CommitTransaction -> smgrDoPendingDeletes -> smgrdounlinkall -> ...
    # -> Invalidate[Local]Buffer.
    node.sql("BEGIN")
    node.sql(create)
    node.sql(insert)
    node.sql("COMMIT")
    node.sql("BEGIN")
    node.sql("SELECT read_rel_block_ll($1, 1, wait_complete=>false)", tbl)
    with no_messages():
        node.sql(f"DROP TABLE {tbl}")
        node.sql("COMMIT")


def test_inject(node):
    """Tests using injection points. Mostly to exercise hard IO errors that are
    hard to trigger without using injection points."""
    skip_unless_injection_points()

    base_err = r'could not read blocks 2\.\.2 in file "base/.*"'

    # injected what we'd expect
    node.sql("SELECT inj_io_short_read_attach(8192)")
    node.sql("SELECT invalidate_rel_block('tbl_ok', 2)")
    with no_messages():
        assert node.sql("SELECT count(*) FROM tbl_ok WHERE ctid = '(2, 1)'") == 1, (
            "injected what we'd expect"
        )

    # injected a read shorter than a single block, expecting error
    node.sql("SELECT inj_io_short_read_attach(17)")
    node.sql("SELECT invalidate_rel_block('tbl_ok', 2)")
    with pytest.raises(LibpqError, match=base_err + ": read only 0 of 8192 bytes"):
        node.sql("SELECT count(*) FROM tbl_ok WHERE ctid = '(2, 1)'")

    inval = [
        f"SELECT invalidate_rel_block('tbl_ok', {b})" for b in (0, 1, 2, 3, 5, 6, 7, 8)
    ]

    # shorten multi-block read to a single block, should retry
    node.sql_batch(*inval)
    node.sql("SELECT inj_io_short_read_attach(8192)")
    with no_messages():
        assert node.sql("SELECT count(*) FROM tbl_ok") == 10000, (
            "shortened multi-block read to a single block, retried"
        )

    # shorten multi-block read to two blocks, should retry
    node.sql_batch(*inval)
    node.sql("SELECT inj_io_short_read_attach(8192*2)")
    with no_messages():
        assert node.sql("SELECT count(*) FROM tbl_ok") == 10000, (
            "shortened multi-block read to two blocks, retried"
        )

    # verify that page verification errors are detected even as part of a
    # shortened multi-block read (tbl_corr, block 1 is corrupted)
    node.sql_batch(
        "SELECT invalidate_rel_block('tbl_corr', 0)",
        "SELECT invalidate_rel_block('tbl_corr', 1)",
        "SELECT invalidate_rel_block('tbl_corr', 2)",
        "SELECT inj_io_short_read_attach(8192)",
    )
    with pytest.raises(
        LibpqError, match=r'invalid page in block 1 of relation "base/.*'
    ):
        node.sql("SELECT count(*) FROM tbl_corr WHERE ctid < '(2, 1)'")

    # trigger a hard error, should error out
    node.sql("SELECT inj_io_short_read_attach(-errno_from_string('EIO'))")
    node.sql("SELECT invalidate_rel_block('tbl_ok', 2)")
    eio = base_err + r": (?:I/O|Input/output) error"
    with pytest.raises(LibpqError, match=eio):
        node.sql("SELECT count(*) FROM tbl_ok")
    with pytest.raises(LibpqError, match=eio):
        node.sql("SELECT count(*) FROM tbl_ok")
    # now the IO should be ok.
    node.sql("SELECT inj_io_short_read_detach()")
    with no_messages():
        assert node.sql("SELECT count(*) FROM tbl_ok") == 10000, (
            "IO is ok again after detaching the injection point"
        )

    # trigger a different hard error, should error out
    node.sql("SELECT inj_io_short_read_attach(-errno_from_string('EROFS'))")
    node.sql("SELECT invalidate_rel_block('tbl_ok', 2)")
    with pytest.raises(LibpqError, match=base_err + ": Read-only file system"):
        node.sql("SELECT count(*) FROM tbl_ok")
    node.sql("SELECT inj_io_short_read_detach()")


def test_inject_worker(node):
    """Tests using injection points, only for io_method=worker.

    io_method=worker has the special case of needing to reopen files. That can
    in theory fail, because the file could be gone. That's a hard path to test
    for real, so we use an injection point to trigger it.
    """
    if node.sql("SHOW io_method") != "worker":
        pytest.skip("worker-specific test")
    skip_unless_injection_points()

    # trigger a failure to reopen, should error out, but should recover
    node.sql("SELECT inj_io_reopen_attach()")
    node.sql("SELECT invalidate_rel_block('tbl_ok', 1)")
    with pytest.raises(
        LibpqError,
        match=r'could not read blocks 1\.\.1 in file "base/.*": No such file or directory',
    ):
        node.sql("SELECT count(*) FROM tbl_ok")
    node.sql("SELECT inj_io_reopen_detach()")
    with no_messages():
        assert node.sql("SELECT count(*) FROM tbl_ok") == 10000, (
            "IO is ok again after detaching the reopen injection point"
        )


def checksum_failures(conn, datname):
    """The (count, last failure time) pair for a database, or for shared
    relations when ``datname`` is None."""
    if datname is None:
        return conn.sql(
            "SELECT checksum_failures, checksum_last_failure "
            "FROM pg_stat_database WHERE datname IS NULL"
        )
    return conn.sql(
        "SELECT checksum_failures, checksum_last_failure "
        "FROM pg_stat_database WHERE datname = $1",
        datname,
    )


def checksum_count(conn, datname):
    return checksum_failures(conn, datname)[0]


def assert_checksum_increased(node, before, datname):
    """Wait until the database's checksum_failures has risen past ``before``
    and a failure timestamp is recorded (stats flush asynchronously).

    The comparison is made here rather than in the polled query so a failure
    reports the count it actually saw.
    """
    for _ in wait_until(f"checksum_failures did not rise above {before}"):
        count, last_failure = checksum_failures(node, datname)
        if count > before and last_failure is not None:
            return


def start_buffer_io(buf, wait):
    return f"SELECT buffer_call_start_io({buf}, for_input=>true, wait=>{wait})"


def terminate_buffer_io(buf, succeed):
    return (
        f"SELECT buffer_call_terminate_io({buf}, for_input=>true, "
        f"succeed=>{succeed}, io_error=>false, release_aio=>false)"
    )


def test_startwait_io(node):
    """Test interplay between StartBufferIO and TerminateBufferIO."""
    a = node.connect()
    b = node.connect()

    # Verify behavior for normal tables

    # create a buffer we can play around with
    buf = a.sql("SELECT buffer_create_toy('tbl_ok', 1)")

    # check that one backend can perform StartBufferIO
    assert a.sql(start_buffer_io(buf, "true")) is True, "first StartBufferIO"

    # but not twice on the same buffer (non-waiting)
    assert a.sql(start_buffer_io(buf, "false")) is False, (
        "second StartBufferIO fails, same session"
    )
    assert b.sql(start_buffer_io(buf, "false")) is False, (
        "second StartBufferIO fails, other session"
    )

    # start io in a different session, will block
    fut = wait_block(node, b, start_buffer_io(buf, "true"), "BufferIo")

    # Terminate the IO, without marking it as success, this should trigger the
    # waiting session to be able to start the io
    a.sql(terminate_buffer_io(buf, "false"))

    # Because the IO was terminated, but not marked as valid, second session
    # should get the right to start io
    assert fut.result() is True, "blocking start buffer io, terminating io, not valid"

    # terminate the IO again
    b.sql(terminate_buffer_io(buf, "false"))

    # same as the above scenario, but mark IO as having succeeded
    assert a.sql(start_buffer_io(buf, "true")) is True, (
        "blocking buffer io w/ success: first start buffer io"
    )

    # start io in a different session, will block
    fut = wait_block(node, b, start_buffer_io(buf, "true"), "BufferIo")

    # Terminate the IO, marking it as success
    a.sql(terminate_buffer_io(buf, "true"))

    # Because the IO was terminated, and marked as valid, second session should
    # complete but not need io
    assert fut.result() is False, "blocking start buffer io, terminating io, valid"

    # buffer is valid now, make it invalid again
    a.sql("SELECT buffer_create_toy('tbl_ok', 1)")

    # Verify behavior for temporary tables
    #
    # Can't unfortunately share the code with the normal table case, there are
    # too many behavioral differences.

    # create a buffer we can play around with
    a.sql("CREATE TEMPORARY TABLE tmp_ok(data int not null)")
    a.sql("INSERT INTO tmp_ok SELECT generate_series(1, 10000)")
    buf = a.sql("SELECT buffer_create_toy('tmp_ok', 3)")

    # check that one backend can perform StartLocalBufferIO
    assert a.sql(start_buffer_io(buf, "false")) is True, "first StartLocalBufferIO"

    # Because local buffers don't use IO_IN_PROGRESS, a second
    # StartLocalBufferIO succeeds as well. This test mostly serves as a
    # documentation of that fact. If we had actually started IO, it'd be
    # different.
    assert a.sql(start_buffer_io(buf, "false")) is True, (
        "second StartLocalBufferIO succeeds, same session"
    )

    # Terminate the IO again, without marking it as a success
    a.sql(terminate_buffer_io(buf, "false"))
    assert a.sql(start_buffer_io(buf, "false")) is True, (
        "StartLocalBufferIO after not marking valid succeeds, same session"
    )

    # Terminate the IO again, marking it as a success
    a.sql(terminate_buffer_io(buf, "true"))

    # Now another StartLocalBufferIO should fail, this time because the buffer
    # is already valid.
    assert a.sql(start_buffer_io(buf, "true")) is False, (
        "StartLocalBufferIO after marking valid fails"
    )

    # The remaining tests don't make sense for temp tables, as they are
    # concerned with multiple sessions interacting with each other.


def test_read_buffers(node):
    """Tests for StartReadBuffers()."""
    a = node.connect()
    b = node.connect()
    a.sql("CREATE TEMPORARY TABLE tmp_ok(data int not null)")
    a.sql("INSERT INTO tmp_ok SELECT generate_series(1, 5000)")

    cols = "blockoff, blocknum, io_reqd, nblocks"
    # io_reqd masked by foreign IO, for the in-progress cases.
    cols_nf = "blockoff, blocknum, io_reqd and not foreign_io, nblocks"

    def read_buffers(table, start, n, c=cols):
        # simplify_result=False so a one-row result still comes back as a list:
        # otherwise the shape of each expected value below would silently encode
        # how many rows it expects.
        # c is a column list, so it stays interpolated; the relation and block
        # numbers are values.
        return a.sql(
            f"SELECT {c} FROM read_buffers($1, $2, $3)",
            table,
            start,
            n,
            simplify_result=False,
        )

    for table in ("tbl_ok", "tmp_ok"):
        # check that consecutive misses are combined into one read
        a.sql("SELECT evict_rel($1)", table)
        assert read_buffers(table, 0, 2) == [(0, 0, True, 2)], (
            f"{table}: read buffers, combine, block 0-1"
        )
        # but if we do it again, i.e. it's in the buffer pool, there will be
        # two operations
        assert read_buffers(table, 0, 2) == [
            (0, 0, False, 1),
            (1, 1, False, 1),
        ], f"{table}: read buffers, doesn't combine hits, block 0-1"
        # Check that a larger read interrupted by a hit works
        assert read_buffers(table, 3, 1) == [(0, 3, True, 1)], (
            f"{table}: read buffers, prep, block 3"
        )
        assert read_buffers(table, 2, 4) == [
            (0, 2, True, 1),
            (1, 3, False, 1),
            (2, 4, True, 2),
        ], f"{table}: read buffers, interrupted by hit on 3, block 2-5"

        # Verify that a read with an initial buffer hit works
        a.sql("SELECT evict_rel($1)", table)
        assert read_buffers(table, 0, 1) == [(0, 0, True, 1)], (
            f"{table}: read buffers, miss, block 0"
        )
        assert read_buffers(table, 0, 1) == [(0, 0, False, 1)], (
            f"{table}: read buffers, hit, block 0"
        )
        assert read_buffers(table, 1, 1) == [(0, 1, True, 1)], (
            f"{table}: read buffers, miss, block 1"
        )
        assert read_buffers(table, 1, 1) == [(0, 1, False, 1)], (
            f"{table}: read buffers, hit, block 1"
        )
        assert read_buffers(table, 0, 2) == [
            (0, 0, False, 1),
            (1, 1, False, 1),
        ], f"{table}: read buffers, hit, block 0-1"
        assert read_buffers(table, 0, 3) == [
            (0, 0, False, 1),
            (1, 1, False, 1),
            (2, 2, True, 1),
        ], f"{table}: read buffers, hit 0-1, miss 2"

        # Verify that a read with an initial miss and trailing buffer hit(s) works
        a.sql("SELECT invalidate_rel_block($1, 0)", table)
        assert read_buffers(table, 0, 3) == [
            (0, 0, True, 1),
            (1, 1, False, 1),
            (2, 2, False, 1),
        ], f"{table}: read buffers, miss 0, hit 1-2"
        a.sql("SELECT invalidate_rel_block($1, 1)", table)
        a.sql("SELECT invalidate_rel_block($1, 2)", table)
        a.sql("SELECT * FROM read_buffers($1, 3, 2)", table)
        assert read_buffers(table, 1, 4) == [
            (0, 1, True, 2),
            (2, 3, False, 1),
            (3, 4, False, 1),
        ], f"{table}: read buffers, miss 1-2, hit 3-4"

        # Verify that we aren't doing reads larger than
        # io_combine_limit. That's just enforced in read_buffers() function,
        # but kinda still worth testing.
        a.sql("SELECT evict_rel($1)", table)
        a.sql("SET io_combine_limit=3")
        assert read_buffers(table, 1, 5) == [
            (0, 1, True, 3),
            (3, 4, True, 2),
        ], f"{table}: read buffers, io_combine_limit has effect"
        a.sql("RESET io_combine_limit")

        # Test encountering buffer IO we started in the first block of the
        # range.
        #
        # Depending on how quick the IO we start completes, the IO might be
        # completed or we "join" the foreign IO. To hide that variability, the
        # query below treats a foreign IO as not having needed to do IO.
        a.sql("SELECT evict_rel($1)", table)
        a.sql("SELECT read_rel_block_ll($1, 1, wait_complete=>false)", table)
        assert read_buffers(table, 1, 3, cols_nf) == [
            (0, 1, False, 1),
            (1, 2, True, 2),
        ], f"{table}: read buffers, in-progress 1, read 1-3"
        # Test in-progress IO in the middle block of the range
        a.sql("SELECT evict_rel($1)", table)
        a.sql("SELECT read_rel_block_ll($1, 2, wait_complete=>false)", table)
        assert read_buffers(table, 1, 3, cols_nf) == [
            (0, 1, True, 1),
            (1, 2, False, 1),
            (2, 3, True, 1),
        ], f"{table}: read buffers, in-progress 2, read 1-3"
        # Test in-progress IO on the last block of the range
        a.sql("SELECT evict_rel($1)", table)
        a.sql("SELECT read_rel_block_ll($1, 3, wait_complete=>false)", table)
        assert read_buffers(table, 1, 3, cols_nf) == [
            (0, 1, True, 2),
            (2, 3, False, 1),
        ], f"{table}: read buffers, in-progress 3, read 1-3"

    # Test start buffer IO will split IO if there's IO in progress. We can't
    # observe this with sync, as that does not start the IO operation in
    # StartReadBuffers().
    table = "tbl_ok"
    fcols = "blockoff, blocknum, io_reqd, foreign_io, nblocks"
    if node.sql("SHOW io_method") != "sync":
        # Because no IO wref was assigned, block 3 should not report foreign IO
        a.sql("SELECT evict_rel($1)", table)
        buf = b.sql("SELECT buffer_create_toy($1, 3)", table)
        b.sql(start_buffer_io(buf, "true"))
        fut = wait_block(
            node,
            a,
            f"SELECT {fcols} FROM read_buffers($1, 1, 5)",
            "BufferIo",
            params=(table,),
            simplify_result=False,
        )
        b.sql(terminate_buffer_io(buf, "false"))
        assert fut.result() == [
            (0, 1, True, False, 2),
            (2, 3, True, False, 3),
        ], "IO was split due to concurrent failed IO"

        # Same as before, except the concurrent IO succeeds this time
        a.sql("SELECT evict_rel($1)", table)
        buf = b.sql("SELECT buffer_create_toy($1, 3)", table)
        b.sql(start_buffer_io(buf, "true"))
        fut = wait_block(
            node,
            a,
            f"SELECT {fcols} FROM read_buffers($1, 1, 5)",
            "BufferIo",
            params=(table,),
            simplify_result=False,
        )
        b.sql(terminate_buffer_io(buf, "true"))
        assert fut.result() == [
            (0, 1, True, False, 2),
            (2, 3, False, False, 1),
            (3, 4, True, False, 2),
        ], "IO was split due to concurrent successful IO"


@pytest.mark.parametrize("persistency", ["normal", "temporary"])
def test_zero(node, persistency):
    """Behavior of ZERO_ON_ERROR and zero_damaged_pages."""
    a = node.connect()
    b = node.connect()

    kind = "" if persistency == "normal" else persistency
    a.sql(f"CREATE {kind} TABLE tbl_zero(id int) WITH (AUTOVACUUM_ENABLED = false)")
    a.sql("INSERT INTO tbl_zero SELECT generate_series(1, 10000)")

    a.sql("SELECT modify_rel_block('tbl_zero', 0, corrupt_header=>true)")

    # A page validity error is reported,
    with pytest.raises(LibpqError, match=r"invalid page in block 0 of relation"):
        a.sql("SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>false)")
    # ... or zeroed with a warning under zero_on_error.
    with pytest.warns(
        PostgresWarning,
        match=r"invalid page in block 0 of relation .*; zeroing out page",
    ):
        a.sql("SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>true)")

    # Once fixed, the block reads cleanly.
    a.sql("SELECT modify_rel_block('tbl_zero', 0, zero=>true)")
    with no_messages():
        a.sql("SELECT read_rel_block_ll('tbl_zero', 0, zero_on_error=>false)")

    # The correct block number is reported for a different block.
    a.sql("SELECT modify_rel_block('tbl_zero', 3, corrupt_header=>true)")
    with pytest.warns(
        PostgresWarning,
        match=r"invalid page in block 3 of relation .*; zeroing out page",
    ):
        a.sql("SELECT read_rel_block_ll('tbl_zero', 3, zero_on_error=>true)")

    # One read reporting multiple invalid blocks.
    a.sql("SELECT modify_rel_block('tbl_zero', 2, corrupt_header=>true)")
    a.sql("SELECT modify_rel_block('tbl_zero', 3, corrupt_header=>true)")
    with pytest.raises(
        LibpqError, match=r"2 invalid pages among blocks 1\.\.4 of relation"
    ) as err:
        a.sql(
            "SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>false)"
        )
    assert err.value.detail == "Block 2 held the first invalid page."
    assert err.value.hint == "See server log for the other 1 invalid block(s)."

    # zeroed via the ZERO_ON_ERROR flag ...
    with pytest.warns(
        PostgresWarning,
        match=r"zeroing out 2 invalid pages among blocks 1\.\.4 of relation",
    ) as caught:
        a.sql(
            "SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>true)"
        )
    warning = caught.pop(PostgresWarning).message
    assert warning.detail == "Block 2 held the first zeroed page."
    assert warning.hint == "See server log for the other 1 zeroed block(s)."

    # ... and via zero_damaged_pages.
    a.sql("BEGIN")
    a.sql("SET LOCAL zero_damaged_pages = true")
    with pytest.warns(
        PostgresWarning,
        match=r"zeroing out 2 invalid pages among blocks 1\.\.4 of relation",
    ) as caught:
        a.sql(
            "SELECT read_rel_block_ll('tbl_zero', 1, nblocks=>4, zero_on_error=>false)"
        )
    warning = caught.pop(PostgresWarning).message
    assert warning.detail == "Block 2 held the first zeroed page."
    assert warning.hint == "See server log for the other 1 zeroed block(s)."
    a.sql("COMMIT")

    # bufmgr IO detects page validity errors.
    a.sql(
        "SELECT invalidate_rel_block('tbl_zero', g.i) FROM generate_series(0, 15) g(i)"
    )
    a.sql("SELECT modify_rel_block('tbl_zero', 3, zero=>true)")
    with pytest.raises(LibpqError, match=r"invalid page in block 2 of relation"):
        a.sql("SELECT count(*) FROM tbl_zero")
    # ... and zeroes them with zero_damaged_pages.
    a.sql("BEGIN")
    a.sql("SET LOCAL zero_damaged_pages = true")
    with pytest.warns(PostgresWarning, match=r"invalid page in block 2 of relation"):
        a.sql("SELECT count(*) FROM tbl_zero")
    a.sql("COMMIT")

    # A page validity error in an IO that session B completes must not be
    # logged visibly to B. Needs cross-session access, so non-temp only.
    if persistency != "temporary":
        a.sql("SELECT modify_rel_block('tbl_zero', 1, corrupt_header=>true)")
        a.sql(
            "SELECT read_rel_block_ll('tbl_zero', 1, wait_complete=>false, "
            "zero_on_error=>true)"
        )
        with no_messages():
            assert b.sql("SELECT count(*) > 0 FROM tbl_zero"), (
                "page validity error is not logged to the completing session"
            )

    a.sql("DROP TABLE tbl_zero")


def test_checksum(node):
    """Checksum failures are detected and reported in the stats."""

    node.sql("CREATE TABLE tbl_normal(id int) WITH (AUTOVACUUM_ENABLED = false)")
    node.sql("INSERT INTO tbl_normal SELECT generate_series(1, 5000)")
    node.sql("SELECT modify_rel_block('tbl_normal', 3, corrupt_checksum=>true)")
    node.sql(
        "CREATE TEMPORARY TABLE tbl_temp(id int) WITH (AUTOVACUUM_ENABLED = false)"
    )
    node.sql("INSERT INTO tbl_temp SELECT generate_series(1, 5000)")
    node.sql("SELECT modify_rel_block('tbl_temp', 3, corrupt_checksum=>true)")
    node.sql("SELECT modify_rel_block('tbl_temp', 4, corrupt_checksum=>true)")

    # A shared rel with invalid pages: pg_shseclabel isn't accessed by default.
    node.sql("SELECT grow_rel('pg_shseclabel', 4)")
    node.sql("SELECT modify_rel_block('pg_shseclabel', 2, corrupt_checksum=>true)")
    node.sql("SELECT modify_rel_block('pg_shseclabel', 3, corrupt_checksum=>true)")

    # normal rel
    before = checksum_count(node, "postgres")
    with pytest.raises(
        LibpqError, match=r'invalid page in block 3 of relation "base/\d+/\d+"'
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_normal', 3, nblocks=>1, zero_on_error=>false)"
        )
    assert_checksum_increased(node, before, "postgres")

    # temp rel
    before = checksum_count(node, "postgres")
    with pytest.raises(
        LibpqError, match=r'invalid page in block 4 of relation "base/\d+/t\d+_\d+"'
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_temp', 4, nblocks=>2, zero_on_error=>false)"
        )
    assert_checksum_increased(node, before, "postgres")

    # shared rel
    before = checksum_count(node, None)
    with pytest.raises(
        LibpqError,
        match=r'2 invalid pages among blocks 2\.\.3 of relation "global/\d+"',
    ) as err:
        node.sql(
            "SELECT read_rel_block_ll('pg_shseclabel', 2, nblocks=>2, zero_on_error=>false)"
        )
    assert err.value.detail == "Block 2 held the first invalid page."
    assert err.value.hint == "See server log for the other 1 invalid block(s)."
    assert_checksum_increased(node, before, None)

    # restore sanity
    node.sql("SELECT modify_rel_block('pg_shseclabel', 1, zero=>true)")
    node.sql("DROP TABLE tbl_normal")


def test_ignore_checksum(node):
    """ignore_checksum_failure handling, including multi-block reads."""

    node.sql("CREATE TABLE tbl_cs_fail(id int) WITH (AUTOVACUUM_ENABLED = false)")
    node.sql("INSERT INTO tbl_cs_fail SELECT generate_series(1, 10000)")
    count_sql = "SELECT count(*) FROM tbl_cs_fail"
    invalidate = "SELECT invalidate_rel_block('tbl_cs_fail', g.i) FROM generate_series(0, 6) g(i)"
    expect = node.sql(count_sql)

    node.sql("SELECT modify_rel_block('tbl_cs_fail', 1, corrupt_checksum=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 5, corrupt_checksum=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 6, corrupt_checksum=>true)")

    # off: a wrong checksum errors.
    node.sql(invalidate)
    with pytest.raises(LibpqError, match=r"invalid page in block"):
        node.sql(count_sql)

    # on: it is ignored with a warning.
    node.sql("SET ignore_checksum_failure=on")
    node.sql(invalidate)
    with pytest.warns(
        PostgresWarning, match=r"ignoring (checksum failure|\d checksum failures)"
    ):
        assert node.sql(count_sql) == expect, (
            "checksum failure ignored with ignore_checksum_failure=on"
        )

    # ignore in a multi-block read still surfaces a real invalid page as ERROR.
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 2, zero=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 4, corrupt_header=>true)")
    with (
        node.log_contains(r"LOG:  ignoring checksum failure", wait=True),
        pytest.warns(PostgresWarning, match=r"ignoring checksum failure in block 3"),
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>false)"
        )
    with pytest.raises(
        LibpqError, match=r'invalid page in block 4 of relation "base/\d+/\d+"'
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_cs_fail', 2, nblocks=>3, zero_on_error=>false)"
        )

    # multi-block read with different problems in different blocks, zeroed.
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 1, zero=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 2, corrupt_checksum=>true)")
    node.sql(
        "SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true, corrupt_header=>true)"
    )
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 4, corrupt_header=>true)")
    node.sql("SELECT modify_rel_block('tbl_cs_fail', 5, corrupt_header=>true)")
    with (
        node.log_contains(
            [
                r"LOG:  ignoring checksum failure in block 2",
                r'LOG:  invalid page in block 3 of relation "base.*"; zeroing out page',
                r'LOG:  invalid page in block 4 of relation "base.*"; zeroing out page',
                r'LOG:  invalid page in block 5 of relation "base.*"; zeroing out page',
            ],
            wait=True,
        ),
        pytest.warns(
            PostgresWarning,
            match=r"zeroing 3 page\(s\) and ignoring 2 checksum failure\(s\) "
            r"among blocks 1\.\.5 of relation",
        ),
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_cs_fail', 1, nblocks=>5, zero_on_error=>true)"
        )

    # both an invalid header and an invalid checksum in one block
    node.sql(
        "SELECT modify_rel_block('tbl_cs_fail', 3, corrupt_checksum=>true, corrupt_header=>true)"
    )
    with pytest.raises(LibpqError, match=r'invalid page in block 3 of relation "'):
        node.sql(
            "SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>false)"
        )
    with pytest.warns(
        PostgresWarning,
        match=r'invalid page in block 3 of relation "base/.*"; zeroing out page',
    ):
        node.sql(
            "SELECT read_rel_block_ll('tbl_cs_fail', 3, nblocks=>1, zero_on_error=>true)"
        )


def test_checksum_createdb(node):
    """Checksum handling when creating a database from one with an invalid
    block (also a minimal cross-database IO check)."""
    node.sql("CREATE DATABASE regression_createdb_source")
    src = node.connect(dbname="regression_createdb_source")
    src.sql("CREATE EXTENSION test_aio")
    src.sql(
        "CREATE TABLE tbl_cs_fail(data int not null) WITH (AUTOVACUUM_ENABLED = false)"
    )
    src.sql("INSERT INTO tbl_cs_fail SELECT generate_series(1, 1000)")
    src.sql("SELECT modify_rel_block('tbl_cs_fail', 1, corrupt_checksum=>true)")
    # Closed explicitly, not left to per-test cleanup: CREATE DATABASE below
    # refuses to copy a template that still has sessions connected to it.
    src.close()

    createdb = (
        "CREATE DATABASE regression_createdb_target "
        "TEMPLATE regression_createdb_source STRATEGY wal_log"
    )

    # An invalid source block fails the create and is accounted for.
    before = checksum_count(node, "regression_createdb_source")
    with pytest.raises(
        LibpqError, match=r'invalid page in block 1 of relation "base/\d+/\d+"'
    ):
        node.sql(createdb)
    assert_checksum_increased(node, before, "regression_createdb_source")

    # Once the source is fixed, the create succeeds.
    src = node.connect(dbname="regression_createdb_source")
    src.sql("SELECT modify_rel_block('tbl_cs_fail', 1, zero=>true)")
    src.close()

    with no_messages():
        node.sql(createdb)


def test_read_buffers_inject(node):
    """StartReadBuffers() recognizing another backend's in-progress IO as
    foreign IO, using injection points to hold an IO in its completion hook."""
    skip_unless_injection_points()
    a = node.connect()
    b = node.connect()
    c = node.connect()
    table = "tbl_ok"
    sync = node.sql("SHOW io_method") == "sync"
    fcols = "blockoff, blocknum, io_reqd, foreign_io, nblocks"

    def configure_wait(blockno):
        """B: Trigger wait in the next AIO read for the given block."""
        b.sql(
            "SELECT inj_io_completion_wait(pid=>pg_backend_pid(), "
            f"relfilenode=>pg_relation_filenode('{table}'), blockno=>{blockno})"
        )

    # Test if a read buffers encounters AIO in progress by another backend, it
    # recognizes that other IO as a foreign IO.
    a.sql("SELECT evict_rel($1)", table)
    configure_wait(1)
    b_fut = wait_block_any_backend(
        node,
        b,
        "SELECT read_rel_block_ll($1, blockno=>1, nblocks=>1)",
        "completion_wait",
        params=(table,),
    )
    a_fut = wait_block(
        node,
        a,
        f"SELECT {fcols} FROM read_buffers($1, 1, 4)",
        "AioIoCompletion",
        params=(table,),
        simplify_result=False,
    )
    # C: Release B from completion hook
    # C: Release B from completion hook
    c.sql("SELECT inj_io_completion_continue()")
    b_fut.result()
    if sync:
        # sync doesn't issue IO below StartReadBuffers(): one combined read.
        assert a_fut.result() == [(0, 1, True, False, 4)], (
            "read 1-3, blocked on in-progress 1, see expected result"
        )
    else:
        # a foreign IO covering block 1, plus one covering blocks 2-4.
        assert a_fut.result() == [
            (0, 1, True, True, 1),
            (1, 2, True, False, 3),
        ], "read 1-3, blocked on in-progress 1, see expected result"

    # Test if a read buffers encounters AIO in progress by another backend, it
    # recognizes that other IO as a foreign IO. This time we encounter the
    # foreign IO multiple times.
    a.sql("SELECT evict_rel($1)", table)
    configure_wait(3)
    b_fut = wait_block_any_backend(
        node,
        b,
        "SELECT read_rel_block_ll($1, blockno=>2, nblocks=>2)",
        "completion_wait",
        params=(table,),
    )
    a_fut = wait_block(
        node,
        a,
        f"SELECT {fcols} FROM read_buffers($1, 0, 4)",
        "AioIoCompletion",
        params=(table,),
        simplify_result=False,
    )
    c.sql("SELECT inj_io_completion_continue()")
    b_fut.result()
    if sync:
        assert a_fut.result() == [(0, 0, True, False, 4)], (
            "read 0-3, blocked on in-progress 2+3, see expected result"
        )
    else:
        # one IO for blocks 0-1, then foreign IOs for blocks 2 and 3.
        assert a_fut.result() == [
            (0, 0, True, False, 2),
            (2, 2, True, True, 1),
            (3, 3, True, True, 1),
        ], "read 0-3, blocked on in-progress 2+3, see expected result"
