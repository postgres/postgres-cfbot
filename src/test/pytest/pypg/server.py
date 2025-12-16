# Copyright (c) 2025, PostgreSQL Global Development Group

from __future__ import annotations

import contextlib
import ctypes
import os
import pathlib
import platform
import re
import shlex
import shutil
import socket
import subprocess
import threading
from collections.abc import Generator, Mapping, Sequence
from concurrent.futures import Future
from typing import Any

from libpq import (
    PGconn,
    PreparedStatement,
)
from libpq import (
    connect as libpq_connect,
)
from libpq import (
    connstr as libpq_connstr,
)

from . import bins, portlock
from ._env import pg_test_timeout_default
from .util import shell_path
from .wait import wait_until

# Database connections land in when neither the caller nor the server's
# default_connection_options names one.
_FALLBACK_DB = "postgres"


def _escape_conf_value(value: object) -> str:
    """Quote and escape ``value`` as a postgresql.conf single-quoted string —
    the inverse of the server's DeescapeQuotedString(), so arbitrary GUC values
    round-trip without hand-escaping. A ``bool`` becomes ``on``/``off`` so
    boolean GUCs can be set with ``True``/``False``.
    """
    if isinstance(value, bool):
        value = "on" if value else "off"
    value = str(value)
    value = value.replace("\\", "\\\\")
    value = value.replace("'", "''")
    value = value.replace("\n", "\\n")
    value = value.replace("\r", "\\r")
    value = value.replace("\t", "\\t")
    value = value.replace("\b", "\\b")
    value = value.replace("\f", "\\f")
    return f"'{value}'"


class FileBackup(contextlib.AbstractContextManager):
    """Context manager that snapshots a file's contents on entry and restores
    them on exit. Used to roll back per-test edits to the server's config
    files; the snapshot is held in memory rather than a sidecar file, so
    nothing is left behind in the data directory.
    """

    def __init__(self, path: pathlib.Path):
        self._path = path
        self._contents: str | None = None

    def __enter__(self) -> FileBackup:
        self._contents = self._path.read_text()
        return self

    def __exit__(self, *exc: object) -> None:
        assert self._contents is not None  # set by __enter__
        self._path.write_text(self._contents)


class PostgresServer:
    """
    Represents a running PostgreSQL server instance with management utilities.
    Provides methods for configuration, user/database creation, and server control.
    """

    def __init__(
        self,
        name: str,
        basedir: pathlib.Path,
        sockdir: pathlib.Path,
        libpq_handle: ctypes.CDLL,
        *,
        hostaddr: str | None = None,
        port: int | None = None,
        initdb_opts: list[str] | None = None,
        from_backup: pathlib.Path | None = None,
        streaming_primary: PostgresServer | None = None,
        allows_streaming: bool | str = False,
        archiving: bool = False,
        restoring: PostgresServer | None = None,
        restoring_standby: bool = True,
        conf: dict[str, Any] | None = None,
    ):
        """
        Initialize a PostgreSQL server instance. Call start() to actually
        start the server.

        Args:
            name: The name of this server instance (for logging purposes)
            basedir: Directory holding everything belonging to this server:
                the data directory (``pgdata``), base backups taken from it
                (``backup``), and archived WAL (``archives``). Must be unique
                per server *and* per test file, since under autoconf's make
                check the whole suite shares one tmp_check directory.
            sockdir: Path to directory for Unix sockets
            libpq_handle: ctypes handle to libpq
            hostaddr: If provided, use this specific address (e.g., "127.0.0.2")
            port: If provided, use this port instead of finding a free one,
                is currently only allowed if hostaddr is also provided
            initdb_opts: Extra arguments to pass to initdb (e.g.
                ["--locale=C", "--encoding=LATIN1"]). When provided the fast
                INITDB_TEMPLATE copy is bypassed and a real initdb is run, since
                the template was created with the default locale/encoding.
                Options from PG_TEST_INITDB_EXTRA_OPTS are added after these.
            from_backup: Path to a base backup (as produced by
                ``PostgresServer.backup()``) to copy into the data directory
                instead of running initdb. Use this to build a standby or a
                point-in-time-recovery node.
            streaming_primary: When building from a backup, the upstream server
                to stream WAL from. Sets ``primary_conninfo`` and creates a
                ``standby.signal`` file so the node starts as a streaming
                standby. Its ``application_name`` is this node's name, so the
                primary can ``wait_for_catchup()`` on it by name.
            allows_streaming: Configure this server as a replication primary
                (``wal_log_hints`` plus generous ``max_wal_senders`` /
                ``max_replication_slots``). Pass ``"logical"`` to set
                ``wal_level = logical`` instead of ``replica``; any other
                truthy value configures plain physical streaming. Without it
                the server runs with ``wal_level = minimal``, so it can't be
                backed up or replicated from.
            archiving: Enable WAL archiving (``archive_mode = on`` plus an
                ``archive_command`` that copies segments into this server's
                ``archive_dir``).
            restoring: When building from a backup, the upstream server whose
                ``archive_dir`` to restore WAL from (sets ``restore_command``).
                By default a ``standby.signal`` is dropped (the node keeps
                replaying archived WAL as a standby); pass
                ``restoring_standby=False`` for a ``recovery.signal`` instead
                (archive recovery that promotes when WAL runs out).
            restoring_standby: See ``restoring``.
            conf: Extra GUC settings (a ``{name: value}`` dict) to append to
                postgresql.conf before the first start. Use for settings that
                must be present at startup (e.g. a recovery target), since
                ``create_pg`` starts the server immediately.
        """

        if hostaddr is None and port is not None:
            raise NotImplementedError("port was provided without hostaddr")

        self.name = name
        # Everything belonging to this server lives under its basedir, so
        # cleanup is a single rmtree and anything a test places next to the
        # datadir (tablespaces, COPY files, ...) is scoped to this server
        # automatically.
        self.basedir = basedir
        self.datadir = basedir / "pgdata"
        self._backup_root = basedir / "backup"
        self.archive_dir = basedir / "archives"
        self.sockdir = sockdir
        self.libpq_handle = libpq_handle
        # The log deliberately lives outside the data directory: pg_basebackup
        # copies unknown files in pgdata, so a log in there would leak the
        # primary's log lines into backups and confuse log searches on nodes
        # built from them.
        self.log = basedir / "postgresql.log"
        self._log_start_pos = 0
        basedir.mkdir(parents=True, exist_ok=True)

        # ExitStack for cleanup callbacks
        self._cleanup_stack = contextlib.ExitStack()

        # Cached connection reused by sql() (see _get_default_conn). Closed
        # (and reopened lazily on next use) via close_default_conn: on
        # stop()/restart(), at per-test teardown, or explicitly by a test
        # after it made the backend die.
        self._default_conn: PGconn | None = None

        # Connection options connect() and everything built on it use when the
        # caller doesn't pass them (see the default_connection_options
        # property). An unset dbname falls back to _FALLBACK_DB.
        self._default_conn_opts: dict[str, Any] = {}

        # Per-test config save/restore state (see _snapshot_conf_if_needed).
        # Tracking is only armed inside start_new_test(); config written during
        # __init__ and by module-scoped setup is deliberately left untracked so
        # that it persists for the lifetime of the server.
        self._track_conf_changes = False
        self._conf_snapshotted = False
        # None until the test applies an edit; then "reload" or "restart" (the
        # strongest apply the test did), which is how the edit is reverted.
        self._conf_restore_mode: str | None = None

        # Determine whether to use Unix sockets
        use_unix_sockets = platform.system() != "Windows" and hostaddr is None

        initdb_extra_opts = os.environ.get("PG_TEST_INITDB_EXTRA_OPTS", "")

        # The backup carries the primary's config; the conf appended below
        # (port, sockets, ...) overrides it since later entries win.
        if from_backup is not None:
            shutil.copytree(from_backup, self.datadir)
            os.chmod(self.datadir, 0o700)
        # Use INITDB_TEMPLATE if available (much faster than running initdb),
        # unless initdb options require a real initdb: the caller's, or
        # PG_TEST_INITDB_EXTRA_OPTS, since the template may not have been made
        # with them.
        elif (
            (initdb_template := os.environ.get("INITDB_TEMPLATE"))
            and not initdb_opts
            and not initdb_extra_opts
            and os.path.isdir(initdb_template)
        ):
            shutil.copytree(initdb_template, self.datadir)
        else:
            if platform.system() == "Windows":
                auth_method = "trust"
            else:
                auth_method = "peer"
            bins.initdb(
                "--no-sync",
                "--auth",
                auth_method,
                "--pgdata",
                self.datadir,
                *(initdb_opts or []),
                *shlex.split(initdb_extra_opts),
            )

        # Figure out which addresses to listen on, then take a port that is
        # free on all of them. Ports come from pypg.portlock, which reserves
        # them the same way (and in the same directory) as the Perl TAP tests,
        # so the two harnesses never pick the same one.
        if hostaddr is not None:
            # Explicit address provided
            addrs: list[str] = [hostaddr]
        elif socket.has_dualstack_ipv6():
            hostaddr = "::1"
            addrs = [hostaddr, "127.0.0.1"]
        else:
            hostaddr = "127.0.0.1"
            addrs = [hostaddr]

        if port is None:
            port = portlock.get_free_port(addrs)
        else:
            portlock.mark_assigned(port)

        # Store the computed values
        self.hostaddr = hostaddr
        self.port = port
        # Including the host to use for connections - either the socket
        # directory or TCP address
        if use_unix_sockets:
            self.host = str(sockdir)
        else:
            self.host = hostaddr

        # Settings that only affect speed or debugging go before TEMP_CONFIG,
        # so that it can override them, and settings that tests rely on go
        # after it. A server made from a backup
        # already has the primary's config, TEMP_CONFIG included.
        self.append_conf(
            fsync=False,
            # A crashed backend should fail the test visibly instead of the
            # postmaster quietly reinitializing and the test carrying on
            # against a freshly recovered server. Tests of crash recovery
            # turn it back on.
            restart_after_crash=False,
            # Say which kind of process wrote each line (client backend,
            # walsender, startup, ...). The PID, which nearly always has the
            # same width within a log, goes first and the backend type is
            # padded to the length of "client backend", so that messages line
            # up for the common processes. No application name: for sessions
            # it would just be [unknown], since pypg sets none.
            log_line_prefix="%m [%p] %-14b ",
            # With the 5s default, tests that repeatedly restart a primary
            # under synchronous replication stall ~5s per reconnect before the
            # standby reattaches, dominating the run time.
            wal_retrieve_retry_interval="500ms",
        )
        if (temp_config := os.environ.get("TEMP_CONFIG")) and from_backup is None:
            with (
                open(temp_config) as src,
                open(self.datadir / "postgresql.conf", "a") as dst,
            ):
                dst.write(f"\n# From TEMP_CONFIG ({temp_config})\n")
                dst.write(src.read())
                dst.write("\n")
        self.append_conf(
            # An empty value disables Unix sockets when using TCP, avoiding
            # lock conflicts.
            unix_socket_directories=sockdir.as_posix() if use_unix_sockets else "",
            listen_addresses=",".join(addrs),
            port=port,
            datestyle="ISO",
            timezone="UTC",
        )

        # A server that isn't set up for streaming gets wal_level = minimal,
        # which takes code paths replica doesn't (skipping WAL for relations
        # created in the same transaction), and makes a test that replicates
        # from it without asking for streaming fail. A server made from a
        # backup keeps the primary's settings.
        if allows_streaming:
            self.append_conf(
                wal_level="logical" if allows_streaming == "logical" else "replica",
                max_wal_senders=10,
                max_replication_slots=10,
                autovacuum_worker_slots=3,
                wal_log_hints=True,
                hot_standby=True,
                # Keep resource use low, so that many servers can run at once.
                shared_buffers="1MB",
                max_connections=10,
                max_wal_size="128MB",
            )
        elif from_backup is None:
            self.append_conf(wal_level="minimal", max_wal_senders=0)
        if platform.system() == "Windows":
            copy_cmd = "copy"
        else:
            copy_cmd = "cp"

        # Configure streaming replication from the primary: set
        # primary_conninfo and drop a standby.signal so the node comes up as a
        # streaming standby.
        if streaming_primary is not None:
            conninfo = streaming_primary.connstr(application_name=self.name)
            self.append_conf(primary_conninfo=conninfo)
            (self.datadir / "standby.signal").touch()

        # Enable WAL archiving. archive_mode is a postmaster GUC, so this must
        # be configured before the first start.
        if archiving:
            os.makedirs(self.archive_dir, exist_ok=True)
            archive_target = shell_path(self.archive_dir / "%f")
            self.append_conf(
                archive_mode=True,
                archive_command=f'{copy_cmd} "%p" "{archive_target}"',
                wal_level="replica",
            )

        # Restore WAL from an upstream server's archive.
        if restoring is not None:
            archive_source = shell_path(restoring.archive_dir / "%f")
            self.append_conf(
                restore_command=f'{copy_cmd} "{archive_source}" "%p"',
            )
            signal = "standby.signal" if restoring_standby else "recovery.signal"
            (self.datadir / signal).touch()

        # Caller-supplied startup config (e.g. a recovery target).
        if conf:
            self.append_conf(**conf)

    def start(self) -> None:
        """Start the server using pg_ctl."""
        self.pg_ctl("start")
        self.pid = self._read_postmaster_pid()

    def _read_postmaster_pid(self) -> int:
        """Read the postmaster PID from the server's postmaster.pid file."""
        with open(self.datadir / "postmaster.pid") as f:
            return int(f.readline().strip())

    def is_running(self) -> bool:
        """Whether the postmaster looks to be running, based on the presence of
        its postmaster.pid file (pg_ctl removes it on a clean stop)."""
        return (self.datadir / "postmaster.pid").exists()

    def reload(self) -> None:
        """Reload postgresql.conf and pg_hba.conf via ``pg_ctl reload`` (SIGHUP).

        Only settings that can change at SIGHUP take effect; postmaster-level
        settings (shared_buffers, archive_mode, ...) need restart().

        When this applies a config edit made during a test (see
        start_new_test), it records that the edit must be reverted with a reload
        at the end of the test. A restart, if the test also did one, takes
        precedence.
        """
        self.pg_ctl("reload")
        if (
            self._track_conf_changes
            and self._conf_snapshotted
            and self._conf_restore_mode is None
        ):
            self._conf_restore_mode = "reload"

    def restart(self, mode: str = "fast") -> None:
        """Restart the server via ``pg_ctl restart`` and refresh the postmaster
        PID.

        When this applies a config edit made during a test (see
        start_new_test), it records that reverting the edit will also need a
        restart, since a reload cannot undo a postmaster-level GUC. (A restart
        before any edit just re-reads the baseline, so it does not count.)
        """
        self.close_default_conn()
        self.pg_ctl("restart", "--mode", mode)
        self.pid = self._read_postmaster_pid()
        if self._track_conf_changes and self._conf_snapshotted:
            self._conf_restore_mode = "restart"

    def promote(self) -> None:
        """Promote a standby/recovery node to a primary, waiting for the
        promotion to finish (pg_ctl promote -w)."""
        self.pg_ctl("promote", "-w")

    def enable_streaming(self, primary: PostgresServer) -> None:
        """Reconfigure this (stopped) node to stream from ``primary`` as a
        standby: set ``primary_conninfo`` and drop a ``standby.signal``. Use it
        to re-attach a former primary as a standby of a newly-promoted node (a
        role swap); call ``start()`` afterwards. The standby's
        ``application_name`` is this node's name so the new primary can
        ``wait_for_catchup()`` on it by name.
        """
        conninfo = primary.connstr(application_name=self.name)
        self.append_conf(primary_conninfo=conninfo)
        (self.datadir / "standby.signal").touch()

    def current_log_position(self) -> int:
        """Get the current end position of the log file."""
        if self.log.exists():
            return self.log.stat().st_size
        return 0

    def reset_log_position(self) -> None:
        """Mark current log position as start for log_content()."""
        self._log_start_pos = self.current_log_position()

    @contextlib.contextmanager
    def start_new_test(self) -> Generator[PostgresServer, None, None]:
        """
        Prepare server for a new test.

        Resets log position and enters a cleanup subcontext. Within that
        subcontext config edits are tracked: the first config edit snapshots
        the server's config files, and they are restored when the test finishes.
        If the test applied its edit (reload/restart), the restore re-applies
        the same way; if it never did, the restore is just the file rollback,
        since the running server never picked the edit up. See
        _snapshot_conf_if_needed.

        The cached sql() connection is dropped first. sql() closes it when the
        cleanup context it was opened in ends, so a connection opened outside a
        test -- by a module-scoped fixture's setup, most likely -- belongs to
        the module and would otherwise be shared by every test in it, carrying
        session state (a GUC, an open transaction, a temporary table) from one
        test into the next. Closing here means each test's first sql() opens a
        connection registered against that test.
        """
        self.reset_log_position()
        self.close_default_conn()
        with self.subcontext():
            self._conf_snapshotted = False
            self._conf_restore_mode = None
            self._track_conf_changes = True
            try:
                yield self
            finally:
                # Disarm before the subcontext unwinds, so the restore callback
                # it runs (via restart()) does not re-escalate the mode.
                self._track_conf_changes = False

    def psql(self, *args: object) -> None:
        """Run psql with the given arguments."""
        bins.psql("-w", *args, server=self)

    def sql(self, query: str, *params: Any, simplify_result: bool = True) -> Any:
        """Execute a SQL query via libpq. Returns simplified results, or a list
        of row tuples with ``simplify_result=False`` (see ``PGconn.sql``).

        Runs on a cached "default" connection reused across calls (see
        ``_get_default_conn``). Two consequences of the reuse:

        - Session state (SET, temp objects, prepared statements) persists
          across sql() calls on the same node. Use ``sql_oneshot()`` or an
          explicit ``connect()`` for a fresh session or non-default connection
          options.
        - If the backend dies underneath the cached connection (a crash,
          ``pg_terminate_backend``), there is deliberately no automatic
          reconnect: the error keeps propagating until the connection is
          explicitly invalidated by ``stop()``/``restart()``, per-test
          teardown, or ``close_default_conn()``.
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        return self._get_default_conn().sql(
            query, *params, simplify_result=simplify_result
        )

    def sql_oneshot(self, query: str, *params: Any, **connection_opts: Any) -> Any:
        """Execute a SQL query on a fresh, single-use connection.

        Any keyword arguments are passed through to ``connect()`` as connection
        options, e.g. ``sql_oneshot(q, dbname="mydb")``. Use this over ``sql()``
        when the query must run in its own session: connecting as another
        user/database, or when the disconnect itself matters (temporary slots
        or objects dropped at session end, stats flushed on exit, picking up a
        reloaded setting immediately, ...).
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        with self.connect(**connection_opts) as conn:
            return conn.sql(query, *params)

    def sql_batch(self, *queries: str) -> list[Any]:
        """Run several statements like consecutive ``sql()`` calls and return a
        list with every statement's simplified result (see ``PGconn.sql_batch``).

        Like ``sql()`` this runs on the cached default connection, so session
        state the batch establishes (SET, temp objects, an unfinished BEGIN)
        persists into later ``sql()``/``sql_batch()`` calls on the node. A
        batch that scopes session state to its final statement (``SET ROLE;
        UPDATE ...``) should use ``sql_batch_oneshot()`` instead, so the state
        dies with the connection.
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        return self._get_default_conn().sql_batch(*queries)

    def sql_batch_oneshot(self, *queries: str, **connection_opts: Any) -> list[Any]:
        """Run several statements on a fresh, single-use connection and return
        a list with every statement's simplified result.

        Any keyword arguments are passed through to ``connect()`` as connection
        options, e.g. ``sql_batch_oneshot(q1, q2, dbname="mydb")``. Use this
        over ``sql_batch()`` when the batch builds up session state that must
        not leak into later calls on the node, or when the disconnect itself
        matters (see ``sql_oneshot``).
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        with self.connect(**connection_opts) as conn:
            return conn.sql_batch(*queries)

    def prepare(self, query: str, *, name: str | None = None) -> PreparedStatement:
        """Parse ``query`` into a named prepared statement on the cached
        default connection and return the ``PreparedStatement`` (see
        ``PGconn.prepare``).

        Like ``sql()`` this uses the connection shared by all default-conn
        methods, so ``stmt.exec()`` sees session state from earlier ``sql()``
        calls, an unconsumed ``stmt.background_exec()`` future blocks
        ``node.sql()``, and the statement survives until the connection is
        invalidated (stop/restart, per-test teardown, ``close_default_conn``).
        """
        return self._get_default_conn().prepare(query, name=name)

    def background_sql(
        self, query: str, *params: Any, simplify_result: bool = True
    ) -> Future[Any]:
        """Dispatch a query that is expected to *block* and return its Future
        (see ``PGconn.background_sql``).

        Runs on the cached default connection, which refuses further queries
        while the future is unconsumed — any ``node.sql()`` before collecting
        ``.result()`` will raise. Use ``background_sql_oneshot()`` or an
        explicit ``connect()`` when the default connection must stay usable
        while the query is blocked.
        """
        return self._get_default_conn().background_sql(
            query, *params, simplify_result=simplify_result
        )

    def background_sql_oneshot(
        self,
        query: str,
        *params: Any,
        simplify_result: bool = True,
        **connection_opts: Any,
    ) -> Future[Any]:
        """Dispatch a blocking query on its own fresh, single-use connection
        and return its Future (see ``PGconn.background_sql``).

        Keyword arguments are passed through to ``connect()`` as connection
        options. Use this over ``background_sql()`` when the test needs
        ``node.sql()`` to keep working while the query is blocked. The
        connection cannot be closed before this returns — the query is still
        running on it — so the worker thread finishes it as soon as the query
        completes. The future must still be consumed (call ``.result()``).
        """
        conn = self.connect(**connection_opts)
        return conn.background_sql(
            query, *params, close_when_done=True, simplify_result=simplify_result
        )

    def _get_default_conn(self) -> PGconn:
        """Return the cached connection used by sql(), opening it lazily on
        first use.

        connect() registers the connection in whatever cleanup stack is
        current (a per-test subcontext when called inside a test), so
        close_default_conn is registered there too: when that context tears
        down, the cache is forgotten along with it and the next sql()
        reconnects."""
        if self._default_conn is None:
            self._default_conn = self.connect()
            self._cleanup_stack.callback(self.close_default_conn)
        return self._default_conn

    @property
    def default_connection_options(self) -> dict[str, Any]:
        """The connection options that ``connect()`` — and so ``sql()``,
        ``poll_query_until()`` and everything else built on it — applies when
        the caller passes none.

        Assigning to it closes the cached ``sql()`` connection, so the next
        query reconnects with the new options::

            pg.sql("CREATE DATABASE mydb")
            pg.default_connection_options = {"dbname": "mydb"}
            pg.sql("CREATE EXTENSION thing")      # runs in mydb

            pg.default_connection_options = {"user": "regress_user"}
            pg.sql("SELECT current_user")         # runs as regress_user

        Use it when a test does most of its work under options other than the
        defaults — in another database, as another role, or with ``options``
        (libpq's PGOPTIONS) — instead of repeating them at every call, or
        opening a connection by hand just to carry them. An explicit keyword to
        ``connect()``/``sql_oneshot()`` still wins, so a test can reach back to
        the default at any point.

        Assignment replaces the whole mapping rather than merging into it; the
        getter returns a copy, so a test that wants to add one option can read,
        update and assign back. ``dbname`` falls back to ``postgres`` whenever
        the mapping does not set it.

        Deliberately *not* applied to connection_env(): that describes the
        server to a client program run with those PG* variables, where silently
        substituting a different database or role would be a surprise.
        """
        return dict(self._default_conn_opts)

    @default_connection_options.setter
    def default_connection_options(self, opts: Mapping[str, Any]) -> None:
        # The cached connection carries the old options, so it has to go for the
        # change to mean anything for sql().
        self.close_default_conn()
        self._default_conn_opts = dict(opts)

    def close_default_conn(self) -> None:
        """Close and forget the cached connection used by sql(), if any; the
        next sql() call opens a fresh one.

        Called by stop()/restart() (the cached session would otherwise outlive
        the backend it was talking to). sql() never reconnects on its own, so
        call this from a test to restore sql() connectivity after the backend
        died some other way — a crash, pg_terminate_backend, ... — once that
        error has been asserted.
        """
        if self._default_conn is not None:
            self._default_conn.close()
            self._default_conn = None

    def append_conf(self, **gucs: object) -> None:
        """Append GUC settings to postgresql.conf.

        Each keyword is written as ``name = 'value'`` with the value escaped
        for a postgresql.conf single-quoted string, so callers never have to
        quote or escape values themselves (a ``bool`` becomes ``on``/``off``).
        For GUCs whose names are not valid Python identifiers — the dotted
        names of extension GUCs — unpack a dict::

            node.append_conf(primary_conninfo=conninfo, work_mem="4MB")
            node.append_conf(**{"basebackup_to_shell.command": cmd})
        """
        self._snapshot_conf_if_needed()
        with open(self.datadir / "postgresql.conf", "a") as f:
            f.writelines(
                f"{name} = {_escape_conf_value(value)}\n"
                for name, value in gucs.items()
            )

    def adjust_conf(self, **gucs: object) -> None:
        """Set each given GUC in postgresql.conf, replacing any existing (or
        commented-out) line for it; a value of ``None`` removes the setting.

        Values are escaped exactly like append_conf, but unlike append_conf
        this leaves a single clean line per setting rather than relying on
        later-line-wins, and does not reload the server::

            node.adjust_conf(work_mem="8MB", fsync=False)
            node.adjust_conf(autovacuum=None)  # remove the setting
            node.adjust_conf(**{"auto_explain.log_min_duration": 0})
        """
        self._snapshot_conf_if_needed()
        path = self.datadir / "postgresql.conf"
        lines = path.read_text().splitlines()
        for setting, value in gucs.items():
            pat = re.compile(rf"^\s*#?\s*{re.escape(setting)}\s*=")
            lines = [ln for ln in lines if not pat.match(ln)]
            if value is not None:
                lines.append(f"{setting} = {_escape_conf_value(value)}")
        path.write_text("\n".join(lines) + "\n")

    def _config_files(self) -> list[pathlib.Path]:
        """The config files that are rolled back between tests."""
        return [
            self.datadir / name
            for name in (
                "postgresql.conf",
                "postgresql.auto.conf",
                "pg_hba.conf",
                "pg_ident.conf",
            )
        ]

    def _snapshot_conf_if_needed(self) -> None:
        """Back up every config file before the first config edit of a test, so
        the edits can be rolled back when the test finishes.

        A no-op unless tracking is armed (start_new_test); config written
        during __init__ or module-scoped setup stays untracked on purpose, so
        it persists for the lifetime of the server. All config files are backed
        up together on the first edit — they are tiny and it keeps a single
        restore point. Only the config-editing helpers arm this, so edits made
        purely through SQL (e.g. ``ALTER SYSTEM``) are not covered.
        """
        if not self._track_conf_changes or self._conf_snapshotted:
            return
        self._conf_snapshotted = True
        # Pushed before the FileBackups so it unwinds last: the files are
        # restored first, and only then does the server reload/restart.
        self._cleanup_stack.callback(self._reapply_conf)
        for path in self._config_files():
            self._cleanup_stack.enter_context(FileBackup(path))

    def _reapply_conf(self) -> None:
        """Bring the server to a running state with the config files just
        restored by the FileBackups in effect. Runs as the last step of a test
        that edited config; see _snapshot_conf_if_needed.
        """
        if not self.is_running():
            # The test left the server stopped; start it so the next test
            # finds a running server. A fresh start reads the restored config
            # in full, so the reload-vs-restart distinction does not apply.
            self.start()
        elif self._conf_restore_mode == "restart":
            self.restart()
        elif self._conf_restore_mode == "reload":
            self.reload()
        # else: the test never applied its edit, so the running server never
        # picked it up; restoring the files is enough.

    def reset_hba(self, database: str, role: str, method: str) -> None:
        """Replace pg_hba.conf with a single local rule and reload the server.

        The rule is written across a continuation line, so every use also
        exercises pg_hba.conf continuation-line parsing.
        """
        self._snapshot_conf_if_needed()
        hba = self.datadir / "pg_hba.conf"
        hba.write_text(f"local {database} {role}\\\n {method}\n")
        self.reload()

    def reset_ident(self, map_name: str, system_user: str, pg_user: str) -> None:
        """Replace pg_ident.conf with a single user-name-map entry and
        reload."""
        self._snapshot_conf_if_needed()
        ident = self.datadir / "pg_ident.conf"
        ident.write_text(f"{map_name} {system_user} {pg_user}\n")
        self.reload()

    def poll_query_until(
        self,
        query: str,
        *params: Any,
        expected: Any = True,
        dbname: str | None = None,
        timeout: float | None = None,
    ) -> Any:
        """Run ``query`` repeatedly until it returns ``expected``.

        Any positional ``params`` after ``query`` are bound to its
        ``$1, $2, ...`` placeholders, like ``sql()``. The comparison is against
        the simplified Python result of ``sql()`` (so ``expected`` is ``True``
        for a boolean ``t`` probe, an ``int`` for a count, a tuple for a
        multi-column row, and so on) rather than psql text. Returns the matching
        result, or raises ``TimeoutError`` once the timeout (defaulting to
        PG_TEST_TIMEOUT_DEFAULT) is exhausted.
        """
        # Close the polling connection on return rather than leaking it until
        # teardown; a lingering connection to ``dbname`` would otherwise block
        # e.g. CREATE DATABASE WITH TEMPLATE on that database.
        with self.connect(dbname=dbname) as conn:
            for _ in wait_until(
                f"query never returned {expected!r}: {query}", timeout=timeout
            ):
                result = conn.sql(query, *params)
                if result == expected:
                    return result

    def pg_ctl(self, *args: object) -> None:
        """Run pg_ctl with the given arguments."""
        # Many tests bounce the server through a raw pg_ctl call instead of
        # stop()/restart(); the cached sql() connection would be left pointing
        # at a dead backend, so invalidate it here too.
        if "stop" in args or "restart" in args:
            self.close_default_conn()
        # --silent drops the "waiting for server to start.... done" progress
        # messages, which end up in every failing test's report without
        # telling anything the echoed command line doesn't. Errors are
        # still printed.
        bins.pg_ctl(
            "--pgdata", self.datadir, "--log", self.log, "--silent", *args, server=self
        )

    def connection_env(self) -> dict[str, str]:
        """Return the PG* environment variables that point a client program at
        this server.

        Use this to run an installed client program (createdb, vacuumdb, ...)
        against this server while capturing its output, e.g. via
        ``vacuumdb(..., server=pg)`` from :mod:`pypg.bins`.
        """
        return {
            "PGHOST": str(self.host),
            "PGPORT": str(self.port),
            "PGDATABASE": "postgres",
            "PGDATA": str(self.datadir),
        }

    def connstr(self, *, dbname: str | None = None, **opts: object) -> str:
        """Return a libpq connection string pointing at this server.

        As in ``connect()``, a ``dbname`` of None means this server's
        ``default_connection_options``. Extra keyword options (e.g.
        ``application_name``) are appended. Used for ``primary_conninfo`` on
        standbys and by replication clients.
        """
        return libpq_connstr(self._conn_opts(dbname=dbname, **opts))

    def backup(
        self,
        backup_name: str = "my_backup",
        backup_options: list[str] | None = None,
    ) -> pathlib.Path:
        """Take a base backup of this (running) server with pg_basebackup.

        The backup is written under a per-server backups directory and the path
        is returned, suitable for passing as ``from_backup`` when creating a
        standby.
        """
        backup_path = self._backup_root / backup_name
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        bins.pg_basebackup(
            "--no-sync",
            "--pgdata",
            backup_path,
            "--host",
            self.host,
            "--port",
            str(self.port),
            "--checkpoint",
            "fast",
            *(backup_options or []),
        )
        return backup_path

    def pg_recvlogical_upto(
        self,
        slot_name: str,
        endpos: str,
        *,
        dbname: str | None = None,
        timeout: float | None = None,
        options: dict[str, str] | None = None,
    ) -> str:
        """Stream a logical slot's changes up to ``endpos`` with pg_recvlogical.

        Runs ``pg_recvlogical --start`` (which confirms the changes it reads,
        advancing the slot) and returns its stdout as text with the trailing
        newline stripped. ``options`` is a dict of plugin output options, each
        passed as ``--option name=value``.
        """
        args = [
            "--slot",
            slot_name,
            "--dbname",
            self.connstr(dbname=dbname),
            "--endpos",
            endpos,
            "--file",
            "-",
            "--no-loop",
            "--start",
        ]
        for k, v in (options or {}).items():
            args.append("--option")
            args.append(f"{k}={v}")
        return bins.pg_recvlogical.capture(*args, timeout=timeout)

    def advance_wal(self, num: int) -> None:
        """Advance WAL by ``num`` segments.

        Emits an empty logical message and forces a segment switch ``num``
        times. ``pg_switch_wal()`` flushes WAL, so ``pg_logical_emit_message()``
        is safe in non-transactional mode.
        """
        with self.connect() as conn:
            for _ in range(num):
                conn.sql("SELECT pg_logical_emit_message(false, '', 'foo')")
                conn.sql("SELECT pg_switch_wal()")

    def _get_insert_lsn(self) -> int:
        """Return the current insert LSN of this server, in bytes."""
        return int(self.sql("SELECT pg_current_wal_insert_lsn() - '0/0'"))

    def emit_wal(self, size: int) -> int:
        """Emit a transactional logical message of ``size`` bytes and return the
        resulting end LSN, in bytes."""
        return int(
            self.sql(
                "SELECT pg_logical_emit_message(true, '', repeat('a', $1)) - '0/0'",
                size,
            )
        )

    def write_wal(
        self, tli: int, lsn: int, segment_size: int, data: bytes
    ) -> pathlib.Path:
        """Write ``data`` (bytes) into the WAL segment file at byte ``lsn`` on
        timeline ``tli``, returning the segment path. Used to corrupt WAL on a
        stopped server."""
        segment = lsn // segment_size
        offset = lsn % segment_size
        path = pathlib.Path(self.datadir) / "pg_wal" / f"{tli:08X}{0:08X}{segment:08X}"
        with open(path, "r+b") as f:
            f.seek(offset)
            f.write(data)
        return path

    def advance_wal_out_of_record_splitting_zone(self, wal_block_size: int) -> int:
        """Advance WAL to a safe distance from the end of a page (enough to fit
        a couple of small records), returning the end LSN in bytes."""
        page_threshold = wal_block_size // 4
        end_lsn = self._get_insert_lsn()
        page_offset = end_lsn % wal_block_size
        while page_offset >= wal_block_size - page_threshold:
            self.emit_wal(page_threshold)
            end_lsn = self._get_insert_lsn()
            page_offset = end_lsn % wal_block_size
        return end_lsn

    def advance_wal_to_record_splitting_zone(self, wal_block_size: int) -> int:
        """Advance WAL so close to the end of a page that an XLogRecordHeader
        would not fit on it, returning the end LSN in bytes."""
        record_header_size = 24
        end_lsn = self._get_insert_lsn()
        page_offset = end_lsn % wal_block_size

        # Get fairly close to the end of a page in big steps.
        while page_offset <= wal_block_size - 512:
            self.emit_wal(wal_block_size - page_offset - 256)
            end_lsn = self._get_insert_lsn()
            page_offset = end_lsn % wal_block_size

        # Calibrate the message size so we can get closer 8 bytes at a time.
        message_size = wal_block_size - 80
        while page_offset <= wal_block_size - record_header_size:
            self.emit_wal(message_size)
            end_lsn = self._get_insert_lsn()
            old_offset = page_offset
            page_offset = end_lsn % wal_block_size
            # Adjust the message size until it causes 8-byte changes in offset,
            # enough to be able to split a record header.
            delta = page_offset - old_offset
            if delta > 8:
                message_size -= 8
            elif delta <= 0:
                message_size += 8
        return end_lsn

    def backup_fs_cold(self, backup_name: str = "cold_backup") -> pathlib.Path:
        """Take a filesystem-level cold backup of this (stopped) server.

        Copies the whole data directory, including WAL, into a per-server
        backups directory and returns the path, suitable for ``from_backup``.
        The server must be stopped, as no attempt is made to handle concurrent
        writes; a node restored from such a backup enters crash recovery before
        switching to archive recovery.
        """
        backup_path = self._backup_root / backup_name
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(
            self.datadir,
            backup_path,
            ignore=shutil.ignore_patterns("postmaster.pid", "postmaster.opts"),
        )
        return backup_path

    def lsn(self, mode: str = "write") -> str:
        """Return a current WAL LSN of this server as a string.

        ``mode`` selects the function: ``insert``/``flush``/``write`` on a
        primary, ``receive``/``replay`` on a standby.
        """
        funcs = {
            "insert": "pg_current_wal_insert_lsn()",
            "flush": "pg_current_wal_flush_lsn()",
            "write": "pg_current_wal_lsn()",
            "receive": "pg_last_wal_receive_lsn()",
            "replay": "pg_last_wal_replay_lsn()",
        }
        return self.sql(f"SELECT {funcs[mode]}")

    def wait_for_catchup(
        self,
        standby_name: str | PostgresServer,
        mode: str = "replay",
        target_lsn: str | None = None,
    ) -> None:
        """Wait until a streaming standby has caught up to ``target_lsn``.

        Polls pg_stat_replication on this (upstream) server until the standby's
        ``<mode>_lsn`` has reached ``target_lsn`` (the upstream's current write
        LSN by default) while in the ``streaming`` state. ``standby_name`` is
        matched against the standby's ``application_name``, which the streaming
        helpers set to the node name.
        """
        if isinstance(standby_name, PostgresServer):
            standby_name = standby_name.name
        if target_lsn is None:
            # On a standby (e.g. a standby acting as a publisher) the write LSN
            # isn't available; use the replay LSN.
            if self.sql("SELECT pg_is_in_recovery()"):
                target_lsn = self.lsn("replay")
            else:
                target_lsn = self.lsn("write")
        # mode names a pg_stat_replication LSN column (sent/write/flush/replay),
        # so it is interpolated as an identifier; the values are bound as params.
        query = (
            f"SELECT $1 <= {mode}_lsn AND state = 'streaming' "
            "FROM pg_catalog.pg_stat_replication "
            "WHERE application_name = $2"
        )
        self.poll_query_until(query, target_lsn, standby_name)

    def wait_for_slot_catchup(
        self,
        slot_name: str,
        mode: str = "restart",
        target_lsn: str | None = None,
    ) -> None:
        """Wait until a replication slot's ``<mode>_lsn`` reaches ``target_lsn``.

        Polls pg_replication_slots on this server. ``mode`` is ``restart`` or
        ``confirmed_flush``.
        """
        assert target_lsn is not None, "target lsn must be specified"
        assert mode in ("restart", "confirmed_flush")
        # mode names a pg_replication_slots LSN column, so it is interpolated as
        # an identifier; the values are bound as params.
        self.poll_query_until(
            f"SELECT $1 <= {mode}_lsn "
            "FROM pg_catalog.pg_replication_slots WHERE slot_name = $2",
            target_lsn,
            slot_name,
        )

    def wait_for_subscription_sync(
        self,
        publisher: PostgresServer | None = None,
        subname: str | None = None,
        dbname: str | None = None,
    ) -> None:
        """Wait for a subscription's initial table sync to finish, then for the
        subscriber to catch up to the publisher.

        Called on the subscriber: polls pg_subscription_rel until every table is
        synced (``r``/``s``). If ``publisher`` and ``subname`` are given, also
        waits for the publisher's walsender (named after the subscription) to
        catch up.
        """
        self.poll_query_until(
            "SELECT count(1) = 0 FROM pg_subscription_rel "
            "WHERE srsubstate NOT IN ('r', 's')",
            dbname=dbname,
        )
        if publisher is not None and subname is not None:
            publisher.wait_for_catchup(subname)

    @contextlib.contextmanager
    def repeat_query(
        self, query: str, interval: float = 0.1, dbname: str | None = None
    ) -> Generator[None, None, None]:
        """Context manager that runs ``query`` repeatedly in the background on
        its own connection until the block exits, like psql's ``\\watch``.

        Used to keep generating activity (e.g. transactions producing running
        xact records) while the test does other work. Errors from a connection
        torn down by a deliberate stop/restart are swallowed.
        """
        stop = threading.Event()
        conn = self.connect(dbname=dbname)

        def loop():
            while not stop.is_set():
                try:
                    conn.sql(query)
                except Exception:
                    return
                stop.wait(interval)

        worker = threading.Thread(target=loop, daemon=True)
        worker.start()
        try:
            yield
        finally:
            stop.set()
            worker.join(timeout=10)
            try:
                conn.close()
            except Exception:
                pass

    def log_standby_snapshot(self, standby: PostgresServer, slot_name: str) -> None:
        """Emit the ``xl_running_xacts`` record a standby's logical slot
        creation is waiting for.

        Called on the primary: waits until the standby slot's ``restart_lsn`` is
        determined, then runs ``pg_log_standby_snapshot()``.
        """
        standby.poll_query_until(
            "SELECT restart_lsn IS NOT NULL FROM pg_catalog.pg_replication_slots "
            "WHERE slot_name = $1",
            slot_name,
        )
        self.sql("SELECT pg_log_standby_snapshot()")

    def create_logical_slot_on_standby(
        self, primary: PostgresServer, slot_name: str, dbname: str | None = None
    ) -> None:
        """Create a logical replication slot on this standby.

        Logical slot creation on a standby blocks until an ``xl_running_xacts``
        record arrives, so it is driven from a background ``pg_recvlogical
        --create-slot`` while the primary is asked to log a standby snapshot.
        """
        recv = subprocess.Popen(
            [
                str(bins.pg_recvlogical.path),
                "--dbname",
                self.connstr(dbname=dbname),
                "--plugin",
                "test_decoding",
                "--slot",
                slot_name,
                "--create-slot",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # Arrange for the xl_running_xacts record pg_recvlogical waits for.
        primary.log_standby_snapshot(self, slot_name)
        recv.wait()
        assert (
            self.sql(
                "SELECT slot_type FROM pg_catalog.pg_replication_slots "
                "WHERE slot_name = $1",
                slot_name,
            )
            == "logical"
        ), f"{slot_name} on standby created"

    def wait_for_event(self, backend_type: str, wait_event: str) -> None:
        """Wait until some backend is parked on a given wait event.

        Polls pg_stat_activity until a backend of ``backend_type`` reports
        ``wait_event``. Use it after dispatching a blocking query with
        ``PGconn.background_sql()`` to confirm it has reached the expected
        wait point.
        """
        self.poll_query_until(
            "SELECT count(*) > 0 FROM pg_stat_activity "
            "WHERE backend_type = $1 AND wait_event = $2",
            backend_type,
            wait_event,
        )

    def wait_for_injection_point(self, name: str) -> None:
        """Wait until some backend is parked at the named injection point.

        Polls pg_stat_activity for a backend whose wait event is the injection
        point (``wait_event_type = 'InjectionPoint'``). Use after dispatching a
        query with ``PGconn.background_sql()`` that is expected to block on
        a point attached in ``'wait'`` mode. Unlike ``wait_for_event()`` it
        does not constrain the backend type, so it also catches background
        workers (autovacuum, checkpointer, ...) parked at the point.
        """
        self.poll_query_until(
            "SELECT count(*) > 0 FROM pg_stat_activity "
            "WHERE wait_event_type = 'InjectionPoint' AND wait_event = $1",
            name,
        )

    @contextlib.contextmanager
    def subcontext(self) -> Generator[PostgresServer, None, None]:
        """
        Create a new cleanup context for per-test isolation.

        Temporarily replaces the cleanup stack so that any cleanup callbacks
        registered within this context will be cleaned up when the context exits.
        """
        old_stack = self._cleanup_stack
        self._cleanup_stack = contextlib.ExitStack()
        try:
            self._cleanup_stack.__enter__()
            yield self
        finally:
            self._cleanup_stack.__exit__(None, None, None)
            self._cleanup_stack = old_stack

    def stop(self, mode: str = "fast") -> None:
        """
        Stop the PostgreSQL server instance.

        Ignores failures if the server is already stopped.
        """
        self.close_default_conn()
        try:
            self.pg_ctl("stop", "--mode", mode)
        except subprocess.CalledProcessError:
            # Server may have already been stopped
            pass

    def log_content(self) -> str:
        """Return log content from the current context's start position."""
        return self.log_since(self._log_start_pos)

    def log_since(self, offset: int) -> str:
        """Return log content written since the given byte offset.

        Pair with current_log_position() to capture exactly the log a single
        operation produces::

            offset = pg.current_log_position()
            conn.sql("...")
            assert "..." in pg.log_since(offset)
        """
        if not self.log.exists():
            return ""
        # Read as bytes and decode leniently: offsets are byte positions, and
        # the server log is not guaranteed to be UTF-8 (e.g. log_connections
        # records a LATIN1 database name verbatim). A strict decode would crash
        # the failure-report hook on such logs.
        with open(self.log, "rb") as f:
            f.seek(offset)
            return f.read().decode("utf-8", errors="replace")

    def wait_for_log(
        self, pattern: str, offset: int = 0, timeout: float | None = None
    ) -> int | None:
        """Wait until the log written since ``offset`` matches ``pattern``.

        Returns the log's end offset once the regex matches, so chained waits
        can continue from there. Raises ``TimeoutError`` otherwise.
        """
        for _ in wait_until(f"log never matched {pattern!r}", timeout=timeout):
            if re.search(pattern, self.log_since(offset)):
                return self.current_log_position()

    @contextlib.contextmanager
    def log_contains(
        self,
        pattern: str | Sequence[str],
        times: int | None = None,
        wait: bool | float = False,
    ) -> Generator[None, None, None]:
        """
        Context manager that checks if the log matches pattern during the block.

        Args:
            pattern: The regex pattern to search for, or a list of patterns
                     that must all match.
            times: If None, any number of matches is accepted.
                   If a number, exactly that many matches of each pattern
                   are required.
            wait: If False, the log is checked once when the block exits,
                  which is enough for lines the backend serving the
                  block's statements writes before replying. For lines
                  written by another process, pass True to wait up to
                  PG_TEST_TIMEOUT_DEFAULT for them, or a number of seconds.
        """
        # Waiting can't show that a line is absent; it would only make the
        # check slower, and pass anyway if the line shows up later.
        if wait is not False and times == 0:
            raise ValueError("log_contains: wait cannot be used with times=0")

        # bool is a subclass of int, so True and False have to be handled
        # before a number is taken as seconds. Not waiting is a timeout of 0,
        # which still checks once.
        if wait is False:
            timeout = 0
        elif wait is True:
            timeout = None
        else:
            timeout = wait
        needed = 1 if times is None else times
        patterns = [pattern] if isinstance(pattern, str) else list(pattern)

        start_pos = self.current_log_position()
        yield
        counts = dict.fromkeys(patterns, 0)
        # Running out of time is reported by the assertions below, the same
        # way as a check that doesn't wait.
        with contextlib.suppress(TimeoutError):
            for _ in wait_until(timeout=timeout):
                content = self.log_since(start_pos)
                counts = {p: len(re.findall(p, content)) for p in patterns}
                if all(c >= needed for c in counts.values()):
                    break
        for p, match_count in counts.items():
            if times is None:
                assert match_count > 0, f"Pattern {p!r} not found in log"
            else:
                assert match_count == times, (
                    f"Expected {times} matches of {p!r}, found {match_count}"
                )

    def cleanup(self) -> None:
        """Run all registered cleanup callbacks."""
        self.close_default_conn()
        self._cleanup_stack.close()

    def _conn_opts(self, **opts: Any) -> dict[str, Any]:
        """The connection options that point at this server, with anything the
        caller passed taking precedence. Shared by connect() and connstr(), so
        a connection and a connection string describing it cannot drift apart.

        An option passed as None means "whatever this server defaults to", so
        the helpers taking an optional dbname (or user, ...) can pass theirs
        straight through instead of resolving the fallback themselves.
        """
        for name, value in self._default_conn_opts.items():
            if opts.get(name) is None:
                opts[name] = value
        if opts.get("dbname") is None:
            opts["dbname"] = _FALLBACK_DB
        opts.setdefault("host", self.host)
        opts.setdefault("port", self.port)
        return opts

    def connect(self, **opts: Any) -> PGconn:
        """
        Creates a connection to this PostgreSQL server instance.

        Args:
            **opts: Additional connection options (can override defaults)

        Returns:
            PGconn: Connected database connection

        Example:
            conn = pg.connect()
            conn = pg.connect(dbname='mydb')
        """
        opts = self._conn_opts(**opts)
        # Only meaningful for a connection this process makes, so it is not
        # part of _conn_opts(): connstr() hands its string to a standby or a
        # client program, which have their own ideas about connect timeouts.
        opts.setdefault("connect_timeout", pg_test_timeout_default())

        return libpq_connect(self.libpq_handle, self._cleanup_stack, **opts)
