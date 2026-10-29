# Copyright (c) 2025, PostgreSQL Global Development Group

from __future__ import annotations

import os
import platform
import shlex
import stat
import subprocess
import sys
from typing import Any


def shell_path(path: str | os.PathLike[str]) -> str:
    """Render ``path`` for embedding in a shell command that the *server* runs
    (archive_command, restore_command, ...). This needs backslashes on
    Windows, even in MinGW environments.

    A plain ``str.replace`` rather than ``pathlib.PureWindowsPath`` because
    the MinGW (MSYS2) Python swaps pathlib's separator (and ``os.sep``) to
    "/", so pathlib/os.path still emit forward slashes there even though
    ``platform.system() == "Windows"`` [1][2]. A literal replace is
    separator-agnostic either way.

    [1] https://bugs.python.org/issue44778
    [2] https://sourceforge.net/p/mingw-w64/bugs/912/
    """
    if platform.system() == "Windows":
        return str(path).replace("/", "\\")
    return str(path)


def eprint(*args: object, **kwargs: Any) -> None:
    """eprint prints to stderr"""
    print(*args, file=sys.stderr, **kwargs)


def check_mode_recursive(
    root: str | os.PathLike[str], dir_mode: int, file_mode: int
) -> list[str]:
    """Check permissions of a directory tree (usually a data directory),
    returning a list of paths whose mode differs from the expected one --
    empty if everything matches, so tests can assert on the result and get
    the offending paths in the failure message. Files that vanish mid-walk
    are ignored: a running server can remove files (e.g. in pg_stat) while we
    are walking.
    """
    violations = []

    def check(path: str, expected: int) -> None:
        try:
            mode = stat.S_IMODE(os.stat(path).st_mode)
        except FileNotFoundError:
            return
        if mode != expected:
            violations.append(f"{path}: mode {oct(mode)} != {oct(expected)}")

    check(os.fspath(root), dir_mode)
    for dirpath, dirnames, filenames in os.walk(root):
        for d in dirnames:
            check(os.path.join(dirpath, d), dir_mode)
        for f in filenames:
            check(os.path.join(dirpath, f), file_mode)
    return violations


def run(
    *command: object,
    check: bool = True,
    shell: bool | None = None,
    silent: bool = False,
    **kwargs: Any,
) -> subprocess.CompletedProcess[Any]:
    """run runs the given command and prints it to stderr"""

    __tracebackhide__ = True  # Don't show in pytest stack traces

    if shell is None:
        shell = len(command) == 1 and isinstance(command[0], str)

    # A shell command is a single string; everything else is a list of
    # stringified argv elements. Build it into a fresh local rather than
    # rebinding the *command parameter (whose static type is a tuple).
    cmd: str | list[str]
    if shell:
        # The shell auto-detection above only sets shell when the single
        # argument is a str; an explicit shell=True is the caller's promise of
        # the same, so command[0] is the shell command line.
        assert isinstance(command[0], str)
        cmd = command[0]
    else:
        cmd = [str(c) for c in command]

    if not silent:
        if shell:
            eprint(f"+ {cmd}")
        else:
            eprint(f"+ {shlex.join(cmd)}")

    if silent:
        kwargs.setdefault("stdout", subprocess.DEVNULL)

    result = subprocess.run(cmd, check=False, shell=shell, **kwargs)

    # Manually throw CalledProcessError to avoid subprocess.run's huge body
    # poluting stack traces.
    if check and result.returncode:
        raise subprocess.CalledProcessError(
            result.returncode, cmd, result.stdout, result.stderr
        )

    return result


def capture(
    command: object,
    *args: object,
    stdout: int = subprocess.PIPE,
    encoding: str = "utf-8",
    **kwargs: Any,
) -> str:
    __tracebackhide__ = True  # Don't pollute pytest stack traces

    return run(
        command, *args, stdout=stdout, encoding=encoding, **kwargs
    ).stdout.removesuffix("\n")
