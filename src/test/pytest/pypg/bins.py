# Copyright (c) 2025, PostgreSQL Global Development Group

"""Callable handles for every installed PostgreSQL program.

Importing any name from this module yields a :class:`~pypg.proc.PgBin` for that
program, so ``from pypg.bins import psql`` (or pg_verifybackup, pg_controldata,
...) works for any installed program without a hardcoded list::

    from pypg.bins import psql, pg_verifybackup
    psql("-c", "select 1")
    pg_verifybackup.check_standard_options()
"""

from __future__ import annotations

import functools

from .proc import PgBin


@functools.cache
def _bin(name: str) -> PgBin:
    return PgBin(name)


def __getattr__(name: str) -> PgBin:
    # PEP 562 module-level __getattr__: any attribute access becomes a cached
    # PgBin for that program name. Guard dunders/privates so importlib,
    # copy/pickle, and "from pypg.bins import _x" probes raise normally rather
    # than fabricating a PgBin("_x").
    #
    # NOTE: we don't use the functools.cache decorator directly on this
    # function, because that confuses Pyright typechecking.
    if name.startswith("_"):
        raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
    return _bin(name)
