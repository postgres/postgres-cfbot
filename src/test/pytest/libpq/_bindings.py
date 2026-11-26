# Copyright (c) 2025, PostgreSQL Global Development Group

"""
Low-level ctypes bindings for libpq.

This is the FFI layer: the opaque struct/pointer types, the enums mirroring
libpq's status codes, the loader that opens the shared library and declares the
function prototypes, and the helper that reads diagnostic fields off a raw
result handle. Nothing here knows about the friendly PGconn/PGresult wrappers
in ``_core``; it only deals in raw libpq handles.
"""

from __future__ import annotations

import ctypes
import enum
import os
import platform
from typing import Any


# PG_DIAG field identifiers from postgres_ext.h
class DiagField(enum.IntEnum):
    SEVERITY = ord("S")
    SEVERITY_NONLOCALIZED = ord("V")
    SQLSTATE = ord("C")
    MESSAGE_PRIMARY = ord("M")
    MESSAGE_DETAIL = ord("D")
    MESSAGE_HINT = ord("H")
    STATEMENT_POSITION = ord("P")
    INTERNAL_POSITION = ord("p")
    INTERNAL_QUERY = ord("q")
    CONTEXT = ord("W")
    SCHEMA_NAME = ord("s")
    TABLE_NAME = ord("t")
    COLUMN_NAME = ord("c")
    DATATYPE_NAME = ord("d")
    CONSTRAINT_NAME = ord("n")
    SOURCE_FILE = ord("F")
    SOURCE_LINE = ord("L")
    SOURCE_FUNCTION = ord("R")


class ConnectionStatus(enum.IntEnum):
    """PostgreSQL connection status codes from libpq."""

    CONNECTION_OK = 0
    CONNECTION_BAD = 1


class ExecStatus(enum.IntEnum):
    """PostgreSQL result status codes from PQresultStatus."""

    PGRES_EMPTY_QUERY = 0
    PGRES_COMMAND_OK = 1
    PGRES_TUPLES_OK = 2
    PGRES_COPY_OUT = 3
    PGRES_COPY_IN = 4
    PGRES_BAD_RESPONSE = 5
    PGRES_NONFATAL_ERROR = 6
    PGRES_FATAL_ERROR = 7
    PGRES_COPY_BOTH = 8
    PGRES_SINGLE_TUPLE = 9
    PGRES_PIPELINE_SYNC = 10
    PGRES_PIPELINE_ABORTED = 11


class _PGconn(ctypes.Structure):
    pass


class _PGresult(ctypes.Structure):
    pass


class _PGnotify(ctypes.Structure):
    """Mirror of libpq's PGnotify (postgres_ext.h). Only the public fields are
    used; ``next`` is libpq-internal and kept opaque."""

    _fields_ = [
        ("relname", ctypes.c_char_p),
        ("be_pid", ctypes.c_int),
        ("extra", ctypes.c_char_p),
        ("next", ctypes.c_void_p),
    ]


_PGconn_p = ctypes.POINTER(_PGconn)
_PGresult_p = ctypes.POINTER(_PGresult)
_PGnotify_p = ctypes.POINTER(_PGnotify)

# Signature of a libpq notice receiver: void (*)(void *arg, const PGresult *res).
_NOTICE_RECEIVER = ctypes.CFUNCTYPE(None, ctypes.c_void_p, _PGresult_p)


def _extract_diag_fields(
    lib: ctypes.CDLL, res: ctypes._Pointer[_PGresult]
) -> dict[str, Any]:
    """Pull the PostgreSQL diagnostic fields off a raw result handle into the
    keyword arguments shared by LibpqError and PostgresMessage. Takes a bare
    _PGresult_p so the notice receiver callback can use it too."""

    def field(diag: DiagField) -> str | None:
        val = lib.PQresultErrorField(res, int(diag))
        return val.decode() if val else None

    position_str = field(DiagField.STATEMENT_POSITION)
    line_str = field(DiagField.SOURCE_LINE)
    return dict(
        sqlstate=field(DiagField.SQLSTATE),
        severity=field(DiagField.SEVERITY),
        primary=field(DiagField.MESSAGE_PRIMARY),
        detail=field(DiagField.MESSAGE_DETAIL),
        hint=field(DiagField.MESSAGE_HINT),
        schema_name=field(DiagField.SCHEMA_NAME),
        table_name=field(DiagField.TABLE_NAME),
        column_name=field(DiagField.COLUMN_NAME),
        datatype_name=field(DiagField.DATATYPE_NAME),
        constraint_name=field(DiagField.CONSTRAINT_NAME),
        internal_query=field(DiagField.INTERNAL_QUERY),
        context=field(DiagField.CONTEXT),
        source_file=field(DiagField.SOURCE_FILE),
        source_line=int(line_str) if line_str else None,
        source_function=field(DiagField.SOURCE_FUNCTION),
        position=int(position_str) if position_str else None,
    )


def load_libpq_handle(
    bindir: str | os.PathLike[str], libdir: str | os.PathLike[str]
) -> ctypes.CDLL:
    """
    Loads a ctypes handle for libpq and declares common function prototypes.

    ``bindir`` and ``libdir`` are pg_config's values, passed in by the caller
    so this module needs no install-discovery dependency of its own.
    """
    system = platform.system()

    if system in ("Linux", "FreeBSD", "NetBSD", "OpenBSD"):
        name = "libpq.so.5"
    elif system == "Darwin":
        name = "libpq.5.dylib"
    elif system == "Windows":
        name = "libpq.dll"
    else:
        assert False, f"the libpq fixture must be updated for {system}"

    if system == "Windows":
        # On Windows, libpq.dll is confusingly in bindir, not libdir. Its
        # dependent DLLs (OpenSSL, zstd, ...) resolve via PATH, which ctypes'
        # default (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) does not search; winmode=0
        # selects the standard PATH-inclusive search instead.
        libpq_path = os.path.join(bindir, name)
        lib = ctypes.CDLL(libpq_path, winmode=0)
    else:
        libpq_path = os.path.join(libdir, name)
        lib = ctypes.CDLL(libpq_path)

    #
    # Function Prototypes
    #

    lib.PQconnectdb.restype = _PGconn_p
    lib.PQconnectdb.argtypes = [ctypes.c_char_p]

    lib.PQstatus.restype = ctypes.c_int
    lib.PQstatus.argtypes = [_PGconn_p]

    lib.PQclosePortal.restype = _PGresult_p
    lib.PQclosePortal.argtypes = [_PGconn_p, ctypes.c_char_p]

    lib.PQclosePrepared.restype = _PGresult_p
    lib.PQclosePrepared.argtypes = [_PGconn_p, ctypes.c_char_p]

    lib.PQresultStatus.restype = ctypes.c_int
    lib.PQresultStatus.argtypes = [_PGresult_p]

    lib.PQclear.restype = None
    lib.PQclear.argtypes = [_PGresult_p]

    lib.PQerrorMessage.restype = ctypes.c_char_p
    lib.PQerrorMessage.argtypes = [_PGconn_p]

    lib.PQfinish.restype = None
    lib.PQfinish.argtypes = [_PGconn_p]

    lib.PQresultErrorMessage.restype = ctypes.c_char_p
    lib.PQresultErrorMessage.argtypes = [_PGresult_p]

    lib.PQntuples.restype = ctypes.c_int
    lib.PQntuples.argtypes = [_PGresult_p]

    lib.PQnfields.restype = ctypes.c_int
    lib.PQnfields.argtypes = [_PGresult_p]

    lib.PQgetvalue.restype = ctypes.c_char_p
    lib.PQgetvalue.argtypes = [_PGresult_p, ctypes.c_int, ctypes.c_int]

    lib.PQgetisnull.restype = ctypes.c_int
    lib.PQgetisnull.argtypes = [_PGresult_p, ctypes.c_int, ctypes.c_int]

    lib.PQftype.restype = ctypes.c_uint
    lib.PQftype.argtypes = [_PGresult_p, ctypes.c_int]

    lib.PQresultErrorField.restype = ctypes.c_char_p
    lib.PQresultErrorField.argtypes = [_PGresult_p, ctypes.c_int]

    _char_pp = ctypes.POINTER(ctypes.c_char_p)

    lib.PQexecParams.restype = _PGresult_p
    lib.PQexecParams.argtypes = [
        _PGconn_p,
        ctypes.c_char_p,  # command
        ctypes.c_int,  # nParams
        ctypes.POINTER(ctypes.c_uint),  # paramTypes (Oid *)
        _char_pp,  # paramValues
        ctypes.c_void_p,  # paramLengths
        ctypes.c_void_p,  # paramFormats
        ctypes.c_int,  # resultFormat
    ]

    lib.PQprepare.restype = _PGresult_p
    lib.PQprepare.argtypes = [
        _PGconn_p,
        ctypes.c_char_p,  # stmtName
        ctypes.c_char_p,  # query
        ctypes.c_int,  # nParams
        ctypes.c_void_p,  # paramTypes
    ]

    lib.PQexecPrepared.restype = _PGresult_p
    lib.PQexecPrepared.argtypes = [
        _PGconn_p,
        ctypes.c_char_p,  # stmtName
        ctypes.c_int,  # nParams
        _char_pp,  # paramValues
        ctypes.c_void_p,  # paramLengths
        ctypes.c_void_p,  # paramFormats
        ctypes.c_int,  # resultFormat
    ]

    lib.PQgetResult.restype = _PGresult_p
    lib.PQgetResult.argtypes = [_PGconn_p]

    lib.PQgetCopyData.restype = ctypes.c_int
    lib.PQgetCopyData.argtypes = [
        _PGconn_p,
        ctypes.POINTER(ctypes.c_char_p),
        ctypes.c_int,
    ]

    lib.PQfreemem.restype = None
    lib.PQfreemem.argtypes = [ctypes.c_void_p]

    lib.PQconsumeInput.restype = ctypes.c_int
    lib.PQconsumeInput.argtypes = [_PGconn_p]

    lib.PQnotifies.restype = _PGnotify_p
    lib.PQnotifies.argtypes = [_PGconn_p]

    lib.PQsetNoticeReceiver.restype = ctypes.c_void_p
    lib.PQsetNoticeReceiver.argtypes = [_PGconn_p, _NOTICE_RECEIVER, ctypes.c_void_p]

    return lib
