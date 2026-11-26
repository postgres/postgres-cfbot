# Copyright (c) 2025, PostgreSQL Global Development Group

"""
libpq testing utilities - ctypes bindings and helpers for PostgreSQL's libpq library.

This module provides Python wrappers around libpq for use in pytest tests.
"""

from __future__ import annotations

from . import errors
from ._bindings import (
    ConnectionStatus,
    DiagField,
    ExecStatus,
    load_libpq_handle,
)
from ._conversions import register_type_info
from ._core import (
    Notify,
    PGconn,
    PGresult,
    PreparedStatement,
    connect,
    connstr,
)
from .errors import (
    LibpqError,
    PostgresMessage,
    PostgresNotice,
    PostgresWarning,
    UnconsumedResultError,
)

__all__ = [
    "ConnectionStatus",
    "DiagField",
    "ExecStatus",
    "LibpqError",
    "Notify",
    "PGconn",
    "PGresult",
    "PostgresMessage",
    "PostgresNotice",
    "PostgresWarning",
    "PreparedStatement",
    "UnconsumedResultError",
    "connect",
    "connstr",
    "errors",
    "load_libpq_handle",
    "register_type_info",
]
