# Copyright (c) 2025, PostgreSQL Global Development Group

from __future__ import annotations

import contextlib
import ctypes
import os
import pathlib
import shutil
import tempfile
from collections.abc import Callable, Generator, Iterator
from typing import Any

import pytest
from libpq import PGconn, load_libpq_handle
from libpq import connect as libpq_connect
from libpq.errors import (
    PostgresDiagnostics,
    UnconsumedResultError,
    add_notes,
    diagnostic_notes,
)

from ._env import pg_test_timeout_default
from .paths import BINDIR, LIBDIR
from .server import PostgresServer

# Stash key for tracking servers for log reporting.
_servers_key = pytest.StashKey[list[PostgresServer]]()

# How much of each server's log a failing test's report includes; see
# pytest_runtest_makereport().
_MAX_REPORTED_LOG_LINES = 30


def _record_server_for_log_reporting(
    request: pytest.FixtureRequest, server: PostgresServer
) -> None:
    """Record a server for log reporting on test failure."""
    if _servers_key not in request.node.stash:
        request.node.stash[_servers_key] = []
    request.node.stash[_servers_key].append(server)


@pytest.fixture(scope="session")
def libpq_handle() -> ctypes.CDLL:
    """
    Loads a ctypes handle for libpq. Some common function prototypes are
    initialized for general use.

    Session-scoped: the loaded library is immutable, process-global state, so
    there is nothing per-module to isolate.
    """
    return load_libpq_handle(BINDIR, LIBDIR)


@pytest.fixture
def connect(libpq_handle: ctypes.CDLL) -> Iterator[Callable[..., PGconn]]:
    """
    Returns a function to connect to PostgreSQL via libpq.

    The returned function accepts connection options as keyword arguments
    (host, port, dbname, etc.) and returns a PGconn object. Connections
    are automatically cleaned up at the end of the test.

    Example:
        conn = connect(host='localhost', port=5432, dbname='postgres')
        result = conn.sql("SELECT 1")
    """
    with contextlib.ExitStack() as stack:

        def _connect(**opts: object) -> PGconn:
            opts.setdefault("connect_timeout", pg_test_timeout_default())
            return libpq_connect(libpq_handle, stack, **opts)

        yield _connect


@pytest.fixture(scope="session")
def tmp_check(tmp_path_factory: pytest.TempPathFactory) -> pathlib.Path:
    """
    Returns the tmp_check directory that should be used for the tests. If
    TESTDATADIR is provided, that will be used; otherwise a new temporary
    directory is created in the pytest temp root.

    Session-scoped: this directory is shared by all test files in a pytest
    invocation (e.g. ``make check`` sets a single TESTDATADIR for the whole
    suite), so everything created in it must have a name that's unique across
    the whole suite. Server basedirs get this via _reserve_basedir below.
    """
    d = os.getenv("TESTDATADIR")
    if d:
        d = pathlib.Path(d)
    else:
        d = tmp_path_factory.mktemp("tmp_check")

    return d


@pytest.fixture(scope="module")
def _reserve_basedir(
    request: pytest.FixtureRequest, tmp_check: pathlib.Path
) -> Iterator[Callable[[str], pathlib.Path]]:
    """
    Returns a function that reserves a basedir for a named server, under which
    the server keeps everything it owns (see Server.__init__). The test file
    name is included in the directory name, so that test files reusing a
    server name never collide inside the suite-wide shared tmp_check directory.

    On teardown the handed-out basedirs are removed again if every test in
    this file passed -- a full run otherwise leaves many gigabytes of cluster
    data behind, slowing down CI. On any failure, or when PG_TEST_NOCLEAN is
    set, the data is kept for debugging. Only this file's
    directories are removed -- not all of tmp_check -- so data kept for an
    earlier failed file survives later passing ones. This runs after the
    dependent server fixtures' teardown has stopped their nodes.
    """
    basedirs: list[pathlib.Path] = []

    def _reserve(name: str) -> pathlib.Path:
        d = tmp_check / f"{request.path.stem}_{name}"
        basedirs.append(d)
        return d

    # Only the Session tracks failures, so detect failures in *this module*
    # as the change in its counter over the fixture's lifetime.
    failed_before = request.session.testsfailed
    yield _reserve

    if (
        request.session.testsfailed == failed_before
        and "PG_TEST_NOCLEAN" not in os.environ
    ):
        for d in basedirs:
            shutil.rmtree(d, ignore_errors=True)


@pytest.fixture(scope="module")
def basedir(_reserve_basedir: Callable[[str], pathlib.Path]) -> pathlib.Path:
    """
    Returns the basedir to use for the pg fixture's server.
    """

    return _reserve_basedir("default")


@pytest.fixture(scope="module")
def sockdir() -> Iterator[pathlib.Path]:
    """
    Returns the directory name to use as the server's unix_socket_directories
    setting. Local client connections use this as the PGHOST.

    Uses tempfile.TemporaryDirectory directly instead of pytest's
    tmp_path_factory, because macOS limits Unix socket paths to 104 bytes
    and pytest's nested temp directories can exceed that. On Linux the limit is
    108 bytes, but the pytest temp root is usually /tmp while on macOS the root
    is much longer.
    """
    with tempfile.TemporaryDirectory(prefix="pytest_postgres_sock") as d:
        yield pathlib.Path(d)


@pytest.fixture(scope="module")
def pg_server_module(
    request: pytest.FixtureRequest,
    basedir: pathlib.Path,
    sockdir: pathlib.Path,
    libpq_handle: ctypes.CDLL,
) -> Iterator[PostgresServer]:
    """
    Starts a running Postgres server for the test module, listening on
    localhost. The HBA initially allows only local UNIX connections from the
    same user.

    Module-scoped rather than session-scoped on purpose: meson runs every test
    file in its own process, so a session never spans more than one module —
    but a local ``pytest pyt/`` run does, and would behave differently if this
    were session-scoped.

    Per-test isolation is a separate concern, handled by the ``pg`` fixture,
    which opens a per-test cleanup subcontext via ``start_new_test()``.

    Returns a PostgresServer instance with methods for server management,
    configuration, and creating test databases/users.
    """
    server = PostgresServer("default", basedir, sockdir, libpq_handle)
    try:
        server.start()
    except Exception:
        # If startup fails the tests never run, so they never get the chance to
        # register the server for log reporting; do it here so the startup logs
        # still make it into the failure report.
        _record_server_for_log_reporting(request, server)
        raise

    yield server

    # Cleanup any test resources, then stop the server.
    server.cleanup()
    server.stop()


@pytest.fixture
def pg(
    request: pytest.FixtureRequest, pg_server_module: PostgresServer
) -> Iterator[PostgresServer]:
    """
    Per-test server context. Use this fixture to make changes to the server
    which will be rolled back at the end of the test (e.g., creating test
    users/databases).

    Also captures the PostgreSQL log position at test start so that any new
    log entries can be included in the test report on failure.
    """
    with pg_server_module.start_new_test() as s:
        _record_server_for_log_reporting(request, s)
        yield s


@pytest.fixture
def create_pg(
    request: pytest.FixtureRequest,
    sockdir: pathlib.Path,
    libpq_handle: ctypes.CDLL,
    _reserve_basedir: Callable[[str], pathlib.Path],
) -> Iterator[Callable[..., PostgresServer]]:
    """
    Factory fixture to create additional PostgreSQL servers (per-test scope).

    Returns a function that creates new PostgreSQL server instances.
    Servers are automatically cleaned up at the end of the test.

    Example:
        def test_multiple_servers(create_pg):
            node1 = create_pg()
            node2 = create_pg()
            node3 = create_pg()
    """
    servers: list[PostgresServer] = []

    def _create(
        name: str | None = None, start: bool = True, **kwargs: Any
    ) -> PostgresServer:
        if name is None:
            count = len(servers) + 1
            name = f"pg{count}"

        basedir = _reserve_basedir(name)
        server = PostgresServer(name, basedir, sockdir, libpq_handle, **kwargs)
        servers.append(server)
        _record_server_for_log_reporting(request, server)
        # Pass start=False when the test must touch the data directory before
        # startup (e.g. drop an extra signal file) or expects startup to fail;
        # call server.start() yourself afterwards.
        if start:
            server.start()
        return server

    yield _create

    for server in servers:
        server.cleanup()
        server.stop()


@pytest.fixture(scope="module")
def _module_scoped_servers() -> list[PostgresServer]:
    """Module-scoped list to track servers created by create_pg_module."""
    return []


@pytest.fixture(scope="module")
def create_pg_module(
    request: pytest.FixtureRequest,
    sockdir: pathlib.Path,
    libpq_handle: ctypes.CDLL,
    _reserve_basedir: Callable[[str], pathlib.Path],
    _module_scoped_servers: list[PostgresServer],
) -> Iterator[Callable[..., PostgresServer]]:
    """
    Factory fixture to create additional PostgreSQL servers (module scope).

    Like create_pg, but servers persist for the entire test module.
    Use this when multiple tests in a module can share the same servers.

    A new per-test subcontext is opened on all servers at the start of each
    test via the _start_module_server_tests autouse fixture.

    Example:
        @pytest.fixture(scope="module")
        def shared_nodes(create_pg_module):
            return [create_pg_module() for _ in range(3)]
    """

    def _create(
        name: str | None = None, start: bool = True, **kwargs: Any
    ) -> PostgresServer:
        if name is None:
            count = len(_module_scoped_servers) + 1
            name = f"pg{count}"
        basedir = _reserve_basedir(name)
        server = PostgresServer(name, basedir, sockdir, libpq_handle, **kwargs)
        _module_scoped_servers.append(server)
        _record_server_for_log_reporting(request, server)
        if start:
            server.start()
        return server

    yield _create

    for server in _module_scoped_servers:
        server.cleanup()
        server.stop()


@pytest.fixture(autouse=True)
def _start_module_server_tests(
    _module_scoped_servers: list[PostgresServer],
) -> Iterator[None]:
    """Opens a per-test subcontext on all module-scoped servers for this test.

    It's hard to reliably detect whether a test uses a module-scoped server or
    not. So this simply assumes all tests in the module use the module-scoped
    servers. There's little harm in registering servers for tests that don't
    use them.
    """
    with contextlib.ExitStack() as stack:
        for server in _module_scoped_servers:
            stack.enter_context(server.start_new_test())
        yield


@pytest.hookimpl(wrapper=True, trylast=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo[None]):
    """
    Adds PostgreSQL server logs to the test report sections.
    """
    report = yield

    session_servers = item.session.stash.get(_servers_key, [])

    module_node = item.getparent(pytest.Module)
    module_servers = module_node.stash.get(_servers_key, []) if module_node else []

    servers = session_servers + module_servers + item.stash.get(_servers_key, [])

    include_name = len(servers) > 1

    for server in servers:
        content = server.log_content()
        lines = content.splitlines()
        # A test's log easily runs to hundreds of lines per server, mostly
        # routine connection and replication messages. The end is where the
        # failure shows up, and the full log stays on disk (and in the CI
        # artifacts), so only show that.
        if len(lines) > _MAX_REPORTED_LOG_LINES:
            omitted = len(lines) - _MAX_REPORTED_LOG_LINES
            content = (
                f"[{omitted} earlier lines omitted, full log: {server.log}]\n"
                + "\n".join(lines[-_MAX_REPORTED_LOG_LINES:])
                + "\n"
            )
        if content.strip():
            section_title = f"Postgres log {report.when}"
            if include_name:
                section_title += f" ({server.name})"
            report.sections.append((section_title, content))
        server.reset_log_position()

    return report


def _notes_on_uncaught_errors() -> Generator[None, object, object]:
    """Hook wrapper body that adds DETAIL, HINT, QUERY, CONTEXT and LOCATION
    notes to a server error escaping a test phase.

    They're often the most useful part of a server error (e.g. "HINT:
    Perhaps you meant to reference the column ...", or where in a function
    it happened), but they aren't part of the message. The notes are only added here, once the error has escaped
    the test, rather than whenever an error is raised, because
    pytest.raises(match=...) also searches notes; an error the test catches
    is thus still matched against just its message.
    """
    __tracebackhide__ = True  # Don't show in pytest stack traces
    try:
        return (yield)
    except PostgresDiagnostics as exc:
        add_notes(exc, diagnostic_notes(exc))
        raise


@pytest.hookimpl(wrapper=True)
def pytest_runtest_setup(item: pytest.Item):
    __tracebackhide__ = True  # Don't show in pytest stack traces
    return (yield from _notes_on_uncaught_errors())


@pytest.hookimpl(wrapper=True)
def pytest_runtest_call(item: pytest.Item):
    __tracebackhide__ = True  # Don't show in pytest stack traces
    return (yield from _notes_on_uncaught_errors())


@pytest.hookimpl(wrapper=True)
def pytest_runtest_teardown(item: pytest.Item, nextitem: pytest.Item | None):
    __tracebackhide__ = True  # Don't show in pytest stack traces
    try:
        return (yield from _notes_on_uncaught_errors())
    except UnconsumedResultError as exc:
        # At teardown this comes from closing the test's connections, so its
        # traceback is fixture and contextlib internals, partly stdlib frames
        # that can't be hidden. The message says where the query came from.
        raise exc.with_traceback(None)
