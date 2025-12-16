# Copyright (c) 2025, PostgreSQL Global Development Group

"""
Install-location discovery for the PostgreSQL build under test.

The paths are constants for the whole session, so they are plain module
globals, filled from a single ``pg_config`` run at import time. pg_config is
found via the ``PG_CONFIG`` environment variable, falling back to ``PATH``.
The cost is that importing this module fails if pg_config can't be run -- but
no test can do anything useful without an install, so failing collection
loudly is fine.
"""

from __future__ import annotations

import os
import pathlib

from .util import capture


def _config_values() -> dict[str, str]:
    """All pg_config settings, from a single argument-less pg_config run, which
    prints every setting as a ``NAME = value`` line."""
    pg_config = os.environ.get("PG_CONFIG", "pg_config")
    values = {}
    for line in capture(pg_config, silent=True).splitlines():
        name, sep, value = line.partition(" = ")
        if sep:
            values[name] = value
    return values


_values = _config_values()

BINDIR = pathlib.Path(_values["BINDIR"])
"""PostgreSQL bin directory (pg_config's ``BINDIR``)."""

LIBDIR = pathlib.Path(_values["LIBDIR"])
"""PostgreSQL lib directory (pg_config's ``LIBDIR``)."""

SHAREDIR = pathlib.Path(_values["SHAREDIR"])
"""PostgreSQL share directory (pg_config's ``SHAREDIR``)."""

INCLUDEDIR_SERVER = pathlib.Path(_values["INCLUDEDIR-SERVER"])
"""PostgreSQL server include directory (pg_config's ``INCLUDEDIR-SERVER``)."""

del _values
