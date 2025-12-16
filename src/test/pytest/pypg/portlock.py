# Copyright (c) 2026, PostgreSQL Global Development Group

"""Port allocation, shared with the Perl TAP tests.

This is a port of ``get_free_port()`` and friends from
``src/test/perl/PostgreSQL/Test/Cluster.pm``, deliberately kept mechanically
identical: the same port range, the same bind probe, and the same
``$portdir/$port.rsv`` lock files in the same directory, resolved the same way.
A pytest run and a prove run regularly happen at the same time, this way the
cooperate together when selecting ports.
"""

from __future__ import annotations

import atexit
import errno
import os
import pathlib
import random
import socket
import sys

# Two things Perl gets from its runtime and Python does not: a portable file
# lock and a way to ask whether a pid is alive. Neither module exists on the
# other platform, so these imports cannot be unconditional.
if sys.platform == "win32":
    import ctypes
    import msvcrt
    from ctypes import wintypes
else:
    import fcntl

# Chosen to sit above the range servers typically use on Unix and below the
# range those systems use for ephemeral client ports (Cluster.pm has the same
# two constants and the same reasoning).
PORT_LOWER_BOUND = 10200
PORT_UPPER_BOUND = 32767

_reservation_files: list[pathlib.Path] = []
# Every port this process has spoken for, so a later search skips it even when
# the server using it is stopped or was never started. Ports that came from
# get_free_port() also have a reservation file; one that a caller picked itself
# does not, which is what this set is really for (see mark_assigned()).
_assigned_ports: set[int] = set()
# Tracking of the last port assigned, to accelerate the search.
_last_port_assigned = random.randint(PORT_LOWER_BOUND, PORT_UPPER_BOUND)

if sys.platform == "win32":

    class _OVERLAPPED(ctypes.Structure):
        """Only ever used zeroed, to lock from offset 0 like Perl does."""

        _fields_ = [
            ("Internal", wintypes.LPVOID),
            ("InternalHigh", wintypes.LPVOID),
            ("Offset", wintypes.DWORD),
            ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE),
        ]

    _LOCKFILE_EXCLUSIVE_LOCK = 0x2
    # The length perl's win32_flock() locks (its LK_LEN), and so the length we
    # have to lock to collide with a prove run holding the same reservation
    # file.
    _LK_LEN = 0xFFFF0000

    # Spelling out the prototypes matters for OpenProcess(): its return value
    # is a HANDLE, which ctypes would otherwise truncate to a C int.
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
    _kernel32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, wintypes.LPDWORD)
    _kernel32.CloseHandle.argtypes = (wintypes.HANDLE,)
    _kernel32.LockFileEx.argtypes = (
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.POINTER(_OVERLAPPED),
    )
    _kernel32.UnlockFileEx.argtypes = (
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.DWORD,
        wintypes.DWORD,
        ctypes.POINTER(_OVERLAPPED),
    )


def _flock_exclusive(fh) -> None:
    """Take an exclusive lock on ``fh``, waiting for as long as it takes."""
    if sys.platform != "win32":
        # It has to be flock(2), the same primitive Perl's flock uses, and not
        # fcntl.lockf(): that is a POSIX record lock, which on Linux does not
        # exclude flock(2) holders, so the TAP tests and these would not lock
        # against each other at all.
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX)
        return

    # Windows has no flock(). Call LockFileEx() instead, with the arguments
    # perl's own flock() emulation uses [1], so that a pytest run and a prove
    # run contend for the same byte range and block on each other rather than
    # each taking a lock the other cannot see.
    #
    # [1] win32_flock() in perl's win32/win32.c:
    #     https://github.com/Perl/perl5/blob/v5.44.0/win32/win32.c#L3084
    overlapped = _OVERLAPPED()
    if not _kernel32.LockFileEx(
        msvcrt.get_osfhandle(fh.fileno()),
        _LOCKFILE_EXCLUSIVE_LOCK,
        0,
        _LK_LEN,
        0,
        ctypes.byref(overlapped),
    ):
        raise ctypes.WinError(ctypes.get_last_error())


def _flock_unlock(fh) -> None:
    """Release the lock taken by _flock_exclusive()."""
    if sys.platform != "win32":
        fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
        return

    # The range has to match the one that was locked exactly, or the lock stays
    # held until the handle is closed.
    overlapped = _OVERLAPPED()
    if not _kernel32.UnlockFileEx(
        msvcrt.get_osfhandle(fh.fileno()),
        0,
        _LK_LEN,
        0,
        ctypes.byref(overlapped),
    ):
        raise ctypes.WinError(ctypes.get_last_error())


def _pid_is_running(pid: int) -> bool:
    """Whether ``pid`` is a live process, the question Cluster.pm asks with
    ``kill 0``."""
    if sys.platform != "win32":
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            # The process exists but is not ours. Cluster.pm treats this as a
            # free port too ("process exists and is owned by us" is what makes
            # it refuse), so behave the same rather than diverging.
            return False
        return True

    # os.kill() cannot be used to probe a pid here: on Windows Python documents
    # that any signal other than CTRL_C_EVENT/CTRL_BREAK_EVENT unconditionally
    # kills the process via TerminateProcess. Probing a reservation must not
    # kill whoever holds it.
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259

    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        # OpenProcess() alone is not enough: the process object outlives the
        # process itself for as long as anybody holds a handle to it, so an
        # exited test runner would keep looking alive and leak its port.
        exit_code = wintypes.DWORD()
        if not _kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
            return False
        return exit_code.value == STILL_ACTIVE
    finally:
        _kernel32.CloseHandle(handle)


def _portdir() -> pathlib.Path:
    """The lock directory, resolved exactly as Cluster.pm resolves it.

    ``PG_TEST_PORT_DIR`` wins (that is what a buildfarm client sets), then a
    ``portlock`` directory at the top of the build tree, and failing that one
    under the test's data directory.
    """
    portdir = os.environ.get("PG_TEST_PORT_DIR")
    if not portdir:
        # PostgreSQL::Test::Utils::tmp_check is TESTDATADIR, or "tmp_check".
        build_dir = os.environ.get("top_builddir") or os.environ.get(
            "TESTDATADIR", "tmp_check"
        )
        portdir = f"{build_dir}/portlock"
    return pathlib.Path(portdir.replace("\\", "/"))


def can_bind(addr: str, port: int) -> bool:
    """Whether ``addr:port`` can be bound and listened on right now."""
    family = socket.AF_INET6 if ":" in addr else socket.AF_INET
    sock = socket.socket(family, socket.SOCK_STREAM)
    try:
        # As in the postmaster (and in Cluster.pm's can_bind), don't use
        # SO_REUSEADDR on Windows, where it would let us bind a port somebody
        # else already has and so report a taken port as free.
        if sys.platform != "win32":
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind((addr, port))
            sock.listen(socket.SOMAXCONN)
        except OSError as e:
            # EADDRNOTAVAIL means the address itself is unusable here, which is
            # not the port's fault; anything else means the port is taken.
            return e.errno == errno.EADDRNOTAVAIL
    finally:
        sock.close()
    return True


def _reserve_port(port: int) -> bool:
    """Claim ``port`` in the lock directory. False if somebody else holds it.

    The file holds the owning pid, so a reservation left behind by a process
    that has since died is reclaimed rather than leaking the port forever.
    """
    filename = _portdir() / f"{port}.rsv"
    # Open read-write so the lock is not lost by reopening, as Cluster.pm notes.
    fd = os.open(filename, os.O_RDWR | os.O_CREAT, 0o644)
    with os.fdopen(fd, "r+") as portfile:
        _flock_exclusive(portfile)
        try:
            try:
                pid = int((portfile.readline() or "0").strip() or "0")
            except ValueError:
                pid = 0
            if pid > 0 and _pid_is_running(pid):
                return False
            portfile.seek(0)
            # Fixed width, so a shorter pid cannot leave trailing junk behind.
            portfile.write(f"{os.getpid():10d}\n")
            portfile.flush()
        finally:
            _flock_unlock(portfile)
    _reservation_files.append(filename)
    return True


def mark_assigned(port: int) -> None:
    """Record that ``port`` is in use by this process, so get_free_port() will
    not hand it out again.

    Only needed for a port that did not come from get_free_port(), i.e. one a
    caller picked itself: that port has no reservation file, so this set is the
    only thing keeping a later search off it.
    """
    _assigned_ports.add(port)


def get_free_port(addrs: list[str] | None = None) -> int:
    """Find a high TCP port nothing is bound to, and reserve it.

    ``addrs`` are the addresses the caller intends to listen on; the port has to
    be free on all of them, plus on 127.0.0.1 so the result is usable for the
    widest range of purposes.

    The reservation lasts until this process exits, whether the server using it
    is running, stopped, or never started at all.
    """
    global _last_port_assigned

    probe = ["127.0.0.1", *(addrs or [])]
    if sys.platform == "win32":
        # Testing 0.0.0.0 covers MSYS, which sets SO_EXCLUSIVEADDRUSE, but is
        # not enough for native Windows, hence the individual addresses too.
        # Cluster.pm probes exactly these, and only there, for the same reason.
        probe += ["127.0.0.2", "127.0.0.3", "0.0.0.0"]
    # Preserve order but drop duplicates.
    probe = list(dict.fromkeys(probe))

    _portdir().mkdir(parents=True, exist_ok=True)

    port = _last_port_assigned
    while True:
        port += 1
        if port > PORT_UPPER_BOUND:
            port = PORT_LOWER_BOUND
        if port in _assigned_ports:
            continue
        if not all(can_bind(addr, port) for addr in probe):
            continue
        if _reserve_port(port):
            _last_port_assigned = port
            mark_assigned(port)
            return port


@atexit.register
def _release_reservations() -> None:
    """Drop this process's reservations, as Cluster.pm's END block does."""
    for filename in _reservation_files:
        try:
            filename.unlink()
        except OSError:
            pass
