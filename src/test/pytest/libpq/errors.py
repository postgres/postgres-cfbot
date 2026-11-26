# Copyright (c) 2025, PostgreSQL Global Development Group

"""
Exception and warning classes for libpq.

Errors the server reports on a failed result are raised as ``LibpqError``;
NOTICE/WARNING/... messages it reports on a successful result are surfaced as
the ``PostgresMessage`` warning categories. Both sides share
``PostgresDiagnostics`` so a caught error or warning exposes the same
``.detail``/``.hint``/``.sqlstate`` fields.
"""

from __future__ import annotations


class PostgresDiagnostics(Exception):
    """Holds the PostgreSQL diagnostic fields (SQLSTATE, detail, hint, ...) the
    server attaches to a result.

    The server sends the same set of fields on an error result as on a
    NOTICE/WARNING result, so this is mixed into both ``LibpqError`` and
    ``PostgresMessage``: a caught notice exposes ``.detail``,
    ``.constraint_name``, etc. exactly like a caught error does.

    It roots at ``Exception`` — a shared base of both ``RuntimeError`` and
    ``UserWarning`` — so that ``super().__init__(message)`` cooperatively
    reaches the real base and stores the message as usual.
    """

    sqlstate: str | None
    severity: str | None
    primary: str | None
    detail: str | None
    hint: str | None
    schema_name: str | None
    table_name: str | None
    column_name: str | None
    datatype_name: str | None
    constraint_name: str | None
    position: int | None
    internal_query: str | None
    context: str | None
    source_file: str | None
    source_line: int | None
    source_function: str | None

    def __init__(
        self,
        message: str,
        *,
        sqlstate: str | None = None,
        severity: str | None = None,
        primary: str | None = None,
        detail: str | None = None,
        hint: str | None = None,
        schema_name: str | None = None,
        table_name: str | None = None,
        column_name: str | None = None,
        datatype_name: str | None = None,
        constraint_name: str | None = None,
        position: int | None = None,
        internal_query: str | None = None,
        context: str | None = None,
        source_file: str | None = None,
        source_line: int | None = None,
        source_function: str | None = None,
    ):
        super().__init__(message)
        self.sqlstate = sqlstate
        self.severity = severity
        self.primary = primary
        self.detail = detail
        self.hint = hint
        self.schema_name = schema_name
        self.table_name = table_name
        self.column_name = column_name
        self.datatype_name = datatype_name
        self.constraint_name = constraint_name
        self.position = position
        self.internal_query = internal_query
        self.context = context
        self.source_file = source_file
        self.source_line = source_line
        self.source_function = source_function


def diagnostic_notes(diag: PostgresDiagnostics) -> list[str]:
    """The parts of a server message that psql prints below its primary
    message (LOCATION only with VERBOSITY verbose), formatted the same way,
    for use as exception notes. QUERY and CONTEXT say where an error
    happened when it doesn't come from the statement itself, e.g. inside a
    function or trigger, and LOCATION which ereport() in the server raised
    it."""
    notes = []
    if diag.detail:
        notes.append(f"DETAIL:  {diag.detail}")
    if diag.hint:
        notes.append(f"HINT:  {diag.hint}")
    if diag.internal_query:
        notes.append(f"QUERY:  {diag.internal_query}")
    if diag.context:
        notes.append(f"CONTEXT:  {diag.context}")
    if diag.source_function or diag.source_file:
        # Like libpq, leave out whatever part the server didn't send.
        location = f"{diag.source_function}, " if diag.source_function else ""
        if diag.source_file and diag.source_line:
            location += f"{diag.source_file}:{diag.source_line}"
        notes.append(f"LOCATION:  {location}")
    return notes


def add_notes(exc: BaseException, notes: list[str]) -> None:
    """Attach ``notes`` to ``exc`` as PEP 678 notes, which pytest prints
    below the exception message."""
    # add_note() only exists on Python 3.11+, but pytest reads __notes__ on
    # older versions too, so set that directly.
    exc.__notes__ = [*getattr(exc, "__notes__", []), *notes]


class LibpqError(PostgresDiagnostics, RuntimeError):
    """Exception for libpq errors with PostgreSQL diagnostic fields."""


class UnconsumedResultError(RuntimeError):
    """A background query's future was not consumed with ``.result()`` before
    its connection was used again or closed.

    Usually raised while a test's connections are closed at teardown, so its
    traceback only shows cleanup code. Its notes say instead which query it
    was, where the test started it, and how it ended.
    """


class PostgresMessage(PostgresDiagnostics, UserWarning):
    """Base category for server messages surfaced over libpq as Python warnings.

    Messages the server sends outside of an error result (what psql prints to
    stderr) are reported as Python warnings, so tests can assert on them with
    ``pytest.warns(..., match=...)``. WARNING and NOTICE map to the subclasses
    below; any other level (INFO, LOG, DEBUG, ...) gets this base category.
    Like ``LibpqError`` it carries the full diagnostic fields (see
    ``PostgresDiagnostics``).
    """


class PostgresNotice(PostgresMessage):
    """A NOTICE message reported by the server over libpq."""


class PostgresWarning(PostgresMessage):
    """A WARNING message reported by the server over libpq."""
