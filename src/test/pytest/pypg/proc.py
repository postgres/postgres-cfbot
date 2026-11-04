# Copyright (c) 2025, PostgreSQL Global Development Group

"""Per-binary callable helpers for invoking installed PostgreSQL programs.

Each installed program is a :class:`PgBin` instance. Calling it is like
:func:`pypg.util.run` (the program
streams to the console and a nonzero exit raises unless ``check=False``);
:meth:`PgBin.capture` is like :func:`pypg.util.capture` (returns the program's
stdout as text). Both accept ``server=`` to point the program at a running
server. Instances come from :mod:`pypg.bins`, not constructed directly::

    from pypg.bins import psql, pg_controldata
    psql("-c", "select 1", server=pg)          # run, raise on failure
    state = pg_controldata.capture(pg.datadir)  # capture stdout
"""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
from collections.abc import Sequence
from typing import TYPE_CHECKING, Any

from . import paths, util

if TYPE_CHECKING:
    from .server import PostgresServer


class PgBin:
    """A single installed PostgreSQL program, resolved against the test bindir."""

    def __init__(self, name: str):
        self.name = name
        # shutil.which rather than Path.exists: it also checks the executable
        # bit, and on Windows resolves the implied .exe suffix.
        resolved = shutil.which(paths.BINDIR / name)
        if resolved is None:
            raise FileNotFoundError(
                f"program {name!r} is not installed in {paths.BINDIR}"
            )
        self.path = pathlib.Path(resolved)

    def __repr__(self) -> str:
        return f"PgBin({self.name!r})"

    def _apply_env(
        self,
        server: PostgresServer | None,
        addenv: dict[str, str] | None,
        kwargs: dict[str, Any],
    ) -> None:
        """Layer a server's PG* connection variables and/or ``addenv`` onto the
        environment the program will run with."""
        if server is None and addenv is None:
            return
        env = kwargs.pop("env", None)
        env = dict(env if env is not None else os.environ)
        if server is not None:
            env.update(server.connection_env())
        if addenv is not None:
            env.update(addenv)
        kwargs["env"] = env

    def __call__(
        self,
        *args: object,
        server: PostgresServer | None = None,
        addenv: dict[str, str] | None = None,
        **kwargs: Any,
    ) -> subprocess.CompletedProcess[Any]:
        """Run the program. Like :func:`pypg.util.run`: output is not captured
        (it streams) and a nonzero exit raises unless ``check=False``. Pass
        ``server=`` to run against a :class:`PostgresServer`."""
        self._apply_env(server, addenv, kwargs)
        return util.run(self.path, *args, **kwargs)

    def capture(
        self,
        *args: object,
        server: PostgresServer | None = None,
        addenv: dict[str, str] | None = None,
        **kwargs: Any,
    ) -> str:
        """Run the program and return its stdout as text (trailing newline
        stripped), like :func:`pypg.util.capture`. Raises on a nonzero exit
        unless ``check=False``."""
        self._apply_env(server, addenv, kwargs)
        return util.capture(self.path, *args, **kwargs)

    def _capture_both(
        self,
        *args: object,
        server: PostgresServer | None = None,
        addenv: dict[str, str] | None = None,
        **kwargs: Any,
    ) -> subprocess.CompletedProcess[str]:
        """Run capturing both stdout and stderr as text (``check=False``),
        returning the CompletedProcess. Backs the ``check_*`` helpers."""
        self._apply_env(server, addenv, kwargs)
        return util.run(
            self.path,
            *args,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
            **kwargs,
        )

    def check_standard_options(self) -> None:
        """Assert the conventions every client program must satisfy.

        ``--help`` and ``--version`` exit 0 writing only to stdout (and --help
        keeps its lines within the length limit), and an unknown option exits
        nonzero with a stderr message. This bundles what nearly every ``src/bin``
        suite checks.
        """
        # The --help convention enforces a maximum line length. This value isn't
        # set in stone; it reflects the current project convention (~80).
        max_help_line = 95

        r = self._capture_both("--help")
        assert r.returncode == 0, r.stderr
        assert r.stdout != "", "--help wrote nothing to stdout"
        assert r.stderr == "", f"--help wrote to stderr: {r.stderr}"
        too_long = [ln for ln in r.stdout.splitlines() if len(ln) > max_help_line]
        assert not too_long, f"--help lines exceed {max_help_line} chars: {too_long}"

        r = self._capture_both("--version")
        assert r.returncode == 0, r.stderr
        assert r.stdout != "", "--version wrote nothing to stdout"
        assert r.stderr == "", f"--version wrote to stderr: {r.stderr}"

        r = self._capture_both("--not-a-valid-option")
        assert r.returncode != 0, "expected nonzero exit for an invalid option"
        assert r.stderr != "", "expected an error message on stderr"

    def check_all(
        self,
        *args: object,
        exit_code: int = 0,
        stdout: str | Sequence[str] = (),
        stderr: str | Sequence[str] = (),
        server: PostgresServer | None = None,
        **kwargs: Any,
    ) -> subprocess.CompletedProcess[str]:
        """Run the program and assert its exit code and output.

        ``stdout``/``stderr`` is a regex -- or an iterable of regexes -- that
        must each be found (``re.search`` with DOTALL and MULTILINE, so ``.``
        spans newlines and ``^``/``$`` match individual lines) in the
        respective stream. Returns the completed process so callers can make
        further assertions.
        """
        if isinstance(stdout, str):
            stdout = (stdout,)
        if isinstance(stderr, str):
            stderr = (stderr,)
        r = self._capture_both(*args, server=server, **kwargs)
        assert r.returncode == exit_code, (
            f"expected exit {exit_code}, got {r.returncode}\n"
            f"stdout: {r.stdout}\nstderr: {r.stderr}"
        )
        for pattern in stdout:
            assert re.search(pattern, r.stdout, re.DOTALL | re.MULTILINE), (
                f"stdout did not match {pattern!r}\nstdout: {r.stdout}"
            )
        for pattern in stderr:
            assert re.search(pattern, r.stderr, re.DOTALL | re.MULTILINE), (
                f"stderr did not match {pattern!r}\nstderr: {r.stderr}"
            )
        return r
