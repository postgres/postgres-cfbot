# Copyright (c) 2025, PostgreSQL Global Development Group

"""
Friendly connection and result wrappers over libpq.

PGconn and PGresult wrap the raw libpq handles from the _bindings module and
turn them into an easy to use API. The ctypes bindings live in _bindings,
value conversion in _conversions, and the server-message warning categories
in messages; this module ties them together.
"""

from __future__ import annotations

import contextlib
import ctypes
import os
import sys
import warnings
from concurrent.futures import Future, ThreadPoolExecutor
from typing import Any, NamedTuple, NoReturn

from ._bindings import (
    _NOTICE_RECEIVER,
    ConnectionStatus,
    DiagField,
    ExecStatus,
    _extract_diag_fields,
    _PGconn,
    _PGresult,
)
from ._conversions import _build_params, _convert_pg_value, simplify_query_results
from .errors import (
    LibpqError,
    PostgresDiagnostics,
    PostgresMessage,
    PostgresNotice,
    PostgresWarning,
    UnconsumedResultError,
    add_notes,
    diagnostic_notes,
)


# A LISTEN/NOTIFY notification, as returned by PGconn.notifies().
class Notify(NamedTuple):
    channel: str
    pid: int
    payload: str


class PGresult(contextlib.AbstractContextManager):
    """Wraps a raw _PGresult_p with a more friendly interface."""

    def __init__(self, lib: ctypes.CDLL, res: ctypes._Pointer[_PGresult]):
        self._lib = lib
        # Cleared to None by __exit__ once the result has been freed.
        self._res: ctypes._Pointer[_PGresult] | None = res

    def __exit__(self, *exc: object) -> None:
        self._lib.PQclear(self._res)
        self._res = None

    def status(self) -> ExecStatus:
        return ExecStatus(self._lib.PQresultStatus(self._res))

    def error_message(self) -> str:
        """Returns the error message associated with this result."""
        msg = self._lib.PQresultErrorMessage(self._res)
        return msg.decode() if msg else ""

    def raise_error(self) -> NoReturn:
        """
        Raises LibpqError with diagnostic information from the result.
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        if not self._res:
            raise LibpqError("query failed: out of memory or connection lost")

        fields = _extract_diag_fields(self._lib, self._res)
        raise LibpqError(fields["primary"] or self.error_message(), **fields)

    def fetch_all(self) -> list[tuple[Any, ...]]:
        """
        Fetch all rows and convert to Python types.
        Returns a list of tuples, with values converted based on their PostgreSQL type.
        """
        nrows = self._lib.PQntuples(self._res)
        ncols = self._lib.PQnfields(self._res)

        # Get type OIDs for each column
        type_oids = [self._lib.PQftype(self._res, col) for col in range(ncols)]

        results = []
        for row in range(nrows):
            row_data = []
            for col in range(ncols):
                if self._lib.PQgetisnull(self._res, row, col):
                    row_data.append(None)
                else:
                    value = self._lib.PQgetvalue(self._res, row, col).decode()
                    row_data.append(_convert_pg_value(value, type_oids[col]))
            results.append(tuple(row_data))

        return results


def _caller_location() -> str:
    """file:line of the code that called into the test framework, i.e. the
    test (or test helper) line that started a background query."""
    frame = sys._getframe(1)
    while frame is not None:
        package = frame.f_globals.get("__name__", "").partition(".")[0]
        if package not in ("libpq", "pypg"):
            return f"{os.path.basename(frame.f_code.co_filename)}:{frame.f_lineno}"
        frame = frame.f_back
    return "an unknown location"


class _MustConsumeFuture(Future):
    """The Future returned by ``PGconn.background_sql()``.

    A plain ``concurrent.futures.Future`` silently drops an exception that
    nobody retrieves, which would let a background query fail unnoticed. This
    subclass records whether ``result()``/``exception()`` was called, so that
    ``PGconn.close()`` can turn a forgotten ``.result()`` into a visible test
    failure instead of a silent pass.

    ``origin`` are exception notes saying what the query is and where the
    test started it, for errors about the future.
    """

    def __init__(self, origin: list[str]) -> None:
        super().__init__()
        self.consumed = False
        self.origin = origin

    def outcome(self) -> list[str]:
        """Exception notes saying how the finished query ended, without
        marking the future consumed."""
        exc = Future.exception(self, timeout=0)
        if exc is None:
            return [f"it returned: {Future.result(self, timeout=0)!r}"]
        notes = [f"it failed with: {type(exc).__name__}: {exc}"]
        if isinstance(exc, PostgresDiagnostics):
            notes += diagnostic_notes(exc)
        return notes

    def result(self, timeout: float | None = None) -> Any:
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self.consumed = True
        try:
            return super().result(timeout)
        except BaseException as e:
            # Its traceback runs through concurrent.futures internals and the
            # worker thread's query code, none of which tells anything about
            # the test, and the stdlib frames can't be hidden like ours. So
            # make it start at the test's .result() call instead.
            raise e.with_traceback(None)

    def exception(self, timeout: float | None = None) -> BaseException | None:
        self.consumed = True
        return super().exception(timeout)


class PGconn(contextlib.AbstractContextManager):
    """
    Wraps a raw _PGconn_p with a more friendly interface. This is just a
    stub; it's expected to grow.
    """

    def __init__(
        self,
        lib: ctypes.CDLL,
        handle: ctypes._Pointer[_PGconn],
        stack: contextlib.ExitStack,
    ):
        self._lib = lib
        # Cleared to None by close() once the connection has been finished.
        self._handle: ctypes._Pointer[_PGconn] | None = handle
        self._stack = stack

        # background_sql() machinery. A single libpq connection must never be
        # driven by two threads at once, so background queries run on one
        # worker thread (created lazily on first use) and only one may be in
        # flight at a time. ``_pending`` is that query's future, if any.
        self._executor: ThreadPoolExecutor | None = None
        self._pending: _MustConsumeFuture | None = None

        # Sequence for prepare()'s generated __p{n} statement names.
        self._prepared_seq = 0

        # Surface NOTICE/WARNING messages as Python warnings so tests can
        # assert on them with pytest.warns(...). The callback object must stay
        # alive as long as the connection, or ctypes frees it and libpq calls
        # into freed memory.
        self._notice_cb = _NOTICE_RECEIVER(self._receive_notice)
        self._lib.PQsetNoticeReceiver(self._handle, self._notice_cb, None)

    def _receive_notice(
        self, _arg: int | None, res: ctypes._Pointer[_PGresult]
    ) -> None:
        severity = self._lib.PQresultErrorField(
            res, int(DiagField.SEVERITY_NONLOCALIZED)
        )
        message = self._lib.PQresultErrorMessage(res)
        # WARNING and NOTICE get their own categories; anything else (INFO, LOG,
        # DEBUG, ...) falls back to the PostgresMessage base.
        category = {
            b"WARNING": PostgresWarning,
            b"NOTICE": PostgresNotice,
        }.get(severity, PostgresMessage)
        # Passing a constructed warning instance (rather than string +
        # category) makes warnings.warn use its type as the category, and lets
        # us attach the same diagnostic fields a LibpqError carries.
        fields = _extract_diag_fields(self._lib, res)
        warnings.warn(category(message.decode().rstrip("\n"), **fields))

    def __exit__(self, *exc: object) -> None:
        # When another exception is already propagating, that is the real
        # failure: abandon any pending background query rather than raising
        # close()'s own "result never consumed" error on top of it.
        if exc[0] is not None:
            self._pending = None
        self.close()

    def close(self) -> None:
        """Close the connection (PQfinish). Idempotent, so it is safe to close
        early even though the owning ExitStack will also close it at teardown.

        Closing with an unconsumed background_sql() future raises a
        RuntimeError."""

        self._check_pending()

        if self._executor is not None:
            self._executor.shutdown(wait=True)
            self._executor = None

        self._close_impl()

    def _close_impl(self) -> None:
        """Release the libpq handle (PQfinish), without close()'s pending
        guard or executor shutdown. Idempotent; also called directly by the
        background_sql(close_when_done=True) worker, which cannot go through
        close() (see background_sql)."""
        if self._handle is not None:
            self._lib.PQfinish(self._handle)
            self._handle = None

    def close_portal(self, name: str) -> None:
        """
        Close a portal, releasing its snapshot. Pass ``""`` for the unnamed
        portal — the one sql()/sql_batch() statements bind.

        Inside an open transaction the unnamed portal survives until the next
        statement, keeping its snapshot registered and the backend's xmin set.
        Call this when a test needs an idle-in-transaction session holding an
        XID but no snapshot — e.g. one that CREATE INDEX CONCURRENTLY's
        WaitForOlderSnapshots must NOT wait for.
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self._check_pending()
        res = self._lib.PQclosePortal(self._handle, name.encode())
        self._result_or_raise(self._stack.enter_context(PGresult(self._lib, res)))

    def sql(self, query: str, *params: Any, simplify_result: bool = True) -> Any:
        """
        Runs ``query`` through the extended query protocol (an unnamed Parse/
        Bind/Execute), the same path real client drivers use, and raises an
        exception if it fails. Any ``params`` are bound to the query's
        ``$1, $2, ...`` placeholders in text format, the libpq equivalent of
        psql's ``<query> \\bind <params> \\g``.

        Returns the query results with automatic type conversion and simplification.
        For commands that don't return data (INSERT, UPDATE, etc.), returns None.

        Examples:
        - SELECT 1 -> 1
        - SELECT 1, 2 -> (1, 2)
        - SELECT * FROM generate_series(1, 3) -> [1, 2, 3]
        - SELECT * FROM (VALUES (1, 'a'), (2, 'b')) t -> [(1, 'a'), (2, 'b')]
        - CREATE TABLE ... -> None
        - INSERT INTO ... -> None

        Pass ``simplify_result=False`` to always get a list of row tuples, with
        no unwrapping and an empty list rather than None. That is what you want
        when the row count is part of what the test is checking, or varies:
        otherwise the caller has to undo the simplification to tell one row from
        one column from no rows at all.

        - SELECT 1 -> [(1,)]
        - SELECT * FROM generate_series(1, 3) -> [(1,), (2,), (3,)]
        - SELECT WHERE false -> []
        - CREATE TABLE ... -> [] (still nothing to return)
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self._check_pending()
        return self._sql_impl(query, *params, simplify_result=simplify_result)

    def _sql_impl(self, query: str, *params: Any, simplify_result: bool = True) -> Any:
        """The actual PQexecParams call behind sql()/background_sql()."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        nparams, types, values = _build_params(params)
        res = self._lib.PQexecParams(
            self._handle, query.encode(), nparams, types, values, None, None, 0
        )
        return self._result_or_raise(
            self._stack.enter_context(PGresult(self._lib, res)),
            simplify_result=simplify_result,
        )

    def _check_pending(self) -> None:
        """Guard run before anything else touches the connection: raises until
        a background_sql() future has been consumed via result()/exception().

        While the query runs the connection is genuinely busy (a second libpq
        call would race the worker thread); once it finishes, raising forces
        the caller to deal with the outcome — including any error — instead of
        letting it silently leak past the next query."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        if self._pending is None:
            return

        if self._pending.consumed:
            self._pending = None
            return

        if self._pending.done():
            err = UnconsumedResultError(
                "the result of a background query was never consumed; call "
                ".result() on its future before using the connection again"
            )
            add_notes(err, self._pending.origin + self._pending.outcome())
        else:
            err = UnconsumedResultError(
                "the connection is still busy with a background query; call "
                ".result() on its future before using the connection again"
            )
            add_notes(err, self._pending.origin)
        raise err

    def background_sql(
        self,
        query: str,
        *params: Any,
        close_when_done: bool = False,
        simplify_result: bool = True,
    ) -> Future[Any]:
        """Dispatch a query that is expected to *block* (on a lock, an
        injection point, ...) and return an already-running Future. The test
        can carry on (e.g. confirm the wait with
        ``PostgresServer.wait_for_event()``, then release it) and collect the
        outcome with ``.result()``, which re-raises any LibpqError.

        The query runs on a worker thread over this same connection rather
        than in a separate process, so the session state it builds up
        (transactions, locks, session-local settings) is visible to later
        calls on this connection. Only one background query at a time: its
        future must be consumed before the connection is used again, or
        _check_pending() raises.

        With ``close_when_done=True`` the worker thread finishes the
        connection (PQfinish) as soon as the query completes, for callers that
        dispatch on a throwaway connection (see
        ``PostgresServer.background_sql_oneshot``). The worker cannot go
        through close() for this — close() rejects an unconsumed future and
        would join the worker's own thread — so it releases the handle
        directly; the must-consume guard still fires at teardown.

        ``simplify_result`` works as it does for ``sql()``: pass False when the
        future's result is a row set whose length matters, so it arrives as a
        list of tuples rather than being unwrapped."""
        return self._dispatch_background(
            lambda: self._sql_impl(query, *params, simplify_result=simplify_result),
            f"query: {query.strip()}",
            close_when_done=close_when_done,
        )

    def _dispatch_background(
        self, work: Any, description: str, *, close_when_done: bool = False
    ) -> Future[Any]:
        """Run ``work`` (a no-argument callable issuing exactly one query on
        this connection) on the worker thread, with the single-pending-query
        bookkeeping shared by background_sql() and
        PreparedStatement.background_exec()."""
        self._check_pending()
        if self._executor is None:
            self._executor = ThreadPoolExecutor(max_workers=1)
        fut = _MustConsumeFuture([description, f"started at: {_caller_location()}"])

        def run():
            if not fut.set_running_or_notify_cancel():
                return
            try:
                fut.set_result(work())
            except BaseException as e:
                fut.set_exception(e)
            finally:
                if close_when_done:
                    self._close_impl()

        self._executor.submit(run)
        self._pending = fut
        return fut

    def sql_batch(self, *queries: str) -> list[Any]:
        """
        Runs each of ``queries`` through the extended query protocol, exactly
        like consecutive sql() calls, and returns a list with every statement's
        simplified result.

        Raises on the first failing statement; earlier statements stay
        executed (and committed).
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self._check_pending()
        return [self._sql_impl(query) for query in queries]

    def notifies(self) -> list[Notify]:
        """
        Return and consume all pending LISTEN/NOTIFY notifications, each a
        ``Notify(channel, pid, payload)``.

        Input is consumed first (``PQconsumeInput``) so notifications already
        waiting on the socket are picked up. A LISTENing session only receives
        notifications once its transaction ends, so call this after the
        relevant command — and poll, since they may arrive slightly after the
        command's own result.
        """
        self._check_pending()
        self._lib.PQconsumeInput(self._handle)
        out = []
        while True:
            n = self._lib.PQnotifies(self._handle)
            if not n:
                break
            c = n.contents
            out.append(Notify(c.relname.decode(), c.be_pid, c.extra.decode()))
            self._lib.PQfreemem(n)
        return out

    def _result_or_raise(self, res: PGresult, *, simplify_result: bool = True) -> Any:
        """Turn a PGresult into a Python value, raising LibpqError on any error
        status. Shared by sql() and the extended-protocol helpers. With
        ``simplify_result=False`` the rows are returned as-is, so callers that
        care about the row count don't have to guess whether a bare value means
        one row, one column, or none."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        status = res.status()
        if status == ExecStatus.PGRES_COMMAND_OK:
            return None if simplify_result else []
        if status == ExecStatus.PGRES_TUPLES_OK:
            rows = res.fetch_all()
            return simplify_query_results(rows) if simplify_result else rows
        if status == ExecStatus.PGRES_COPY_OUT:
            # Drain the COPY OUT stream. An error raised mid-copy surfaces as
            # PQgetCopyData returning -2 with the real result coming from
            # PQgetResult, so resolve that result (raising on error) before
            # handing back the copied bytes — undecoded, since COPY can stream
            # binary data.
            chunks = []
            buf = ctypes.c_char_p()
            while True:
                # Read exactly the returned length; COPY data is not
                # guaranteed to be NUL-terminated.
                n = self._lib.PQgetCopyData(self._handle, ctypes.byref(buf), 0)
                if n <= 0:
                    break
                chunks.append(ctypes.string_at(buf, n))
                self._lib.PQfreemem(buf)
                buf = ctypes.c_char_p()
            final = self._lib.PQgetResult(self._handle)
            self._result_or_raise(self._stack.enter_context(PGresult(self._lib, final)))
            return b"".join(chunks)
        res.raise_error()

    def prepare(self, query: str, *, name: str | None = None) -> PreparedStatement:
        """
        Parse ``query`` into a named prepared statement and return a
        ``PreparedStatement`` to run it with. This is the libpq equivalent of
        psql's ``<query> \\parse <name>``.

        Pass ``name=`` only when the test cares about the statement's name
        (e.g. it appears in a log line or pg_prepared_statements); otherwise a
        connection-unique ``__p{n}`` name is generated.
        """
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self._check_pending()
        if name is None:
            self._prepared_seq += 1
            name = f"__p{self._prepared_seq}"
        res = self._lib.PQprepare(self._handle, name.encode(), query.encode(), 0, None)
        self._result_or_raise(self._stack.enter_context(PGresult(self._lib, res)))
        return PreparedStatement(self, name)

    def _exec_prepared_impl(self, name: str, *params: Any) -> Any:
        """The PQexecPrepared call behind PreparedStatement.exec()/
        background_exec(); like _sql_impl it runs without the pending guard so
        the worker thread can use it."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        # PQexecPrepared takes no paramTypes: the types were fixed at Parse.
        nparams, _types, values = _build_params(params)
        res = self._lib.PQexecPrepared(
            self._handle, name.encode(), nparams, values, None, None, 0
        )
        return self._result_or_raise(
            self._stack.enter_context(PGresult(self._lib, res))
        )


class PreparedStatement(contextlib.AbstractContextManager):
    """A named server-side prepared statement, created by ``PGconn.prepare()``.

    Execute it with ``exec(*params)`` (or ``background_exec(*params)`` when
    the execution is expected to block), and release it with ``close()`` — or
    use it as a context manager to do that automatically. The statement lives
    on the connection that prepared it, so all methods are subject to that
    connection's single-pending-query rule.
    """

    def __init__(self, conn: PGconn, name: str):
        self._conn = conn
        self.name = name
        self._closed = False

    def exec(self, *params: Any) -> Any:
        """Bind ``params`` to the statement and execute it, returning
        simplified results like ``PGconn.sql()``. This is the libpq equivalent
        of psql's ``\\bind_named <name> <params> \\g``."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        self._conn._check_pending()
        return self._conn._exec_prepared_impl(self.name, *params)

    def background_exec(self, *params: Any) -> Future[Any]:
        """Execute the statement on the connection's worker thread and return
        a Future, with the same semantics and must-consume rule as
        ``PGconn.background_sql()``."""
        return self._conn._dispatch_background(
            lambda: self._conn._exec_prepared_impl(self.name, *params),
            f"prepared statement: {self.name}",
        )

    def close(self) -> None:
        """Release the prepared statement (the protocol-level Close message,
        like DEALLOCATE). Idempotent, and a no-op if the connection itself is
        already gone — the statement died with its session."""
        __tracebackhide__ = True  # Don't show in pytest stack traces
        if self._closed:
            return
        self._closed = True
        if self._conn._handle is None:
            return
        self._conn._check_pending()
        res = self._conn._lib.PQclosePrepared(self._conn._handle, self.name.encode())
        self._conn._result_or_raise(
            self._conn._stack.enter_context(PGresult(self._conn._lib, res))
        )

    def __exit__(self, *exc: object) -> None:
        # Mirror PGconn.__exit__: when an exception is already propagating,
        # don't let close()'s own errors (e.g. an unconsumed background future
        # on the connection) mask it.
        if exc[0] is not None:
            self._closed = True
            return
        self.close()


def connstr(opts: dict[str, Any]) -> str:
    """
    Flattens the provided options into a libpq connection string. Values
    are converted to str and quoted/escaped as necessary.
    """
    settings: list[str] = []

    for k, v in opts.items():
        v = str(v)
        if not v:
            v = "''"
        else:
            v = v.replace("\\", "\\\\")
            v = v.replace("'", "\\'")

            # libpq ends an unquoted value at the first whitespace of any kind
            # (not just a space), so wrap in single quotes whenever the value
            # contains any whitespace.
            if any(c.isspace() for c in v):
                v = f"'{v}'"

        settings.append(f"{k}={v}")

    return " ".join(settings)


def connect(
    libpq_handle: ctypes.CDLL,
    stack: contextlib.ExitStack,
    **opts: Any,
) -> PGconn:
    """
    Connects to a server using the given libpq connection options (host, port,
    dbname, connect_timeout, ...) and returns a PGconn registered in ``stack``
    for cleanup. Raises LibpqError if the connection fails.
    """

    conn_p = libpq_handle.PQconnectdb(connstr(opts).encode())

    if libpq_handle.PQstatus(conn_p) != ConnectionStatus.CONNECTION_OK:
        error_msg = libpq_handle.PQerrorMessage(conn_p).decode()
        libpq_handle.PQfinish(conn_p)
        raise LibpqError(error_msg)

    return stack.enter_context(PGconn(libpq_handle, conn_p, stack=stack))
