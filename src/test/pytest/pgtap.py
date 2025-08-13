# Copyright (c) 2025, PostgreSQL Global Development Group

"""
A pytest plugin that reports results as TAP, the format meson's test runner
understands. It is enabled with -p pgtap, which meson does for every pytest
test it runs.

The plugin takes over the standard streams at startup: pytest's own output is
written to a log file under TESTLOGDIR, and the original stdout carries the
TAP stream, with failure details going to stderr where meson shows them. That
also means a test that prints does not corrupt the protocol.
"""

from __future__ import annotations

import os
import sys
from typing import TYPE_CHECKING, Any

import pytest

if TYPE_CHECKING:
    from _pytest._code.code import ExceptionRepr

#
# Helpers
#


class TAP:
    """
    A basic API for reporting via the TAP 12 protocol[1].

    Reporting a newer version like "TAP version 14"[2] here is not even
    possible, despite every meson version accepting it, because meson requires
    that line to be the literal first line of output and testwrap prints a
    comment like this before it:

        # executing test ...

    We could of course modify testwrap to report TAP version 14, but that would
    not buy us much currently. The main feature that could be useful to us is
    subtests, but no meson version implements those[3]: before meson 1.0.0
    their indented lines were hard parse errors that failed the test, and since
    then[4] they are simply ignored with a warning.

    [1] https://testanything.org/tap-specification.html
    [2] https://testanything.org/tap-version-14-specification.html
    [3] https://github.com/mesonbuild/meson/issues/15768
    [4] https://github.com/mesonbuild/meson/commit/d0054f2c3c3497e22069d1efb5b1d985d75fe5ca
    """

    def __init__(self) -> None:
        self.count = 0

    def expect(self, num: int) -> None:
        self.print(f"1..{num}")

    def print(self, *args: Any) -> None:
        print(*args, file=sys.__stdout__)

    def ok(self, name: str) -> None:
        self.count += 1
        self.print("ok", self.count, "-", name)

    def skip(self, name: str, reason: str) -> None:
        self.count += 1
        self.print("ok", self.count, "-", name, "# skip", reason)

    def fail(self, name: str, details: str) -> None:
        self.count += 1
        self.print("not ok", self.count, "-", name)

        # mtest has some odd behavior around TAP tests where it won't print
        # diagnostics on failure if they're part of the stdout stream, so we
        # might as well just dump the details directly to stderr instead.
        print(details, file=sys.__stderr__)


tap = TAP()


class TestNotes:
    """
    Annotations for a single test. The existing pytest hooks keep interesting
    information somewhat separated across the different stages
    (setup/test/teardown), so this class is used to correlate them.
    """

    skipped: bool = False
    skip_reason: str | None = None

    failed: bool = False
    details: str | None = None


# Register a custom key in the stash dictionary for keeping our TestNotes.
notes_key = pytest.StashKey[TestNotes]()


#
# Hook Implementations
#


@pytest.hookimpl(tryfirst=True)
def pytest_configure(config: pytest.Config) -> None:
    """
    Hijacks the standard streams as soon as possible during pytest startup. The
    pytest-formatted output gets logged to file instead, and we'll use the
    original sys.__stdout__/__stderr__ streams for the TAP protocol.
    """
    logdir = os.getenv("TESTLOGDIR")
    if not logdir:
        raise RuntimeError("pgtap requires the TESTLOGDIR envvar to be set")

    os.makedirs(logdir)
    logpath = os.path.join(logdir, "pytest.log")
    # Deliberately not closed: this file replaces the standard streams for the
    # remaining lifetime of the process.
    sys.stdout = sys.stderr = open(logpath, "a", buffering=1)  # noqa: SIM115


@pytest.hookimpl(trylast=True)
def pytest_sessionfinish(
    session: pytest.Session, exitstatus: int | pytest.ExitCode
) -> None:
    """
    Suppresses nonzero exit codes due to failed tests. (In that case, we want
    Meson to report a failure count, not a generic ERROR.)
    """
    if exitstatus == pytest.ExitCode.TESTS_FAILED:
        session.exitstatus = pytest.ExitCode.OK


@pytest.hookimpl
def pytest_collectreport(report: pytest.CollectReport) -> None:
    # Include collection failures directly in Meson error output.
    if report.failed:
        print(report.longreprtext, file=sys.__stderr__)


@pytest.hookimpl
def pytest_internalerror(
    excrepr: ExceptionRepr, excinfo: pytest.ExceptionInfo[BaseException]
) -> None:
    # Include internal errors directly in Meson error output.
    print(excrepr, file=sys.__stderr__)


#
# Hook Wrappers
#
# In pytest parlance, a "wrapper" for a hook can inspect and optionally modify
# existing hooks' behavior, but it does not replace the hook chain. This is done
# through a generator-style API which chains the hooks together (see the use of
# `yield`).
#


@pytest.hookimpl(wrapper=True)
def pytest_collection(session: pytest.Session):
    """Reports the number of gathered tests after collection is finished."""
    result = yield
    tap.expect(session.testscollected)
    return result


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item: pytest.Item, call: pytest.CallInfo[None]):
    """
    Annotates a test item with our TestNotes and grabs relevant information for
    reporting.

    This is called multiple times per test, so it's not correct to print the TAP
    result here. (A test and its teardown stage can both fail, and we want to
    see the details for both.) We instead combine all the information for use by
    our pytest_runtest_protocol wrapper later on.
    """
    report = yield

    if notes_key not in item.stash:
        item.stash[notes_key] = TestNotes()
    notes = item.stash[notes_key]

    if report.passed:
        pass  # no annotation needed

    elif report.skipped:
        notes.skipped = True
        _, _, notes.skip_reason = report.longrepr

    elif report.failed:
        notes.failed = True

        # The first failing report (a test and its teardown can both fail)
        # writes the header; later ones append to what is already there.
        details = notes.details or "{:_^72}\n\n".format(f" {report.head_line} ")

        if report.when in ("setup", "teardown"):
            details += "\n{:_^72}\n\n".format(
                f" Error during {report.when} of {report.head_line} "
            )

        details += report.longreprtext + "\n"

        # Include captured stdout/stderr/log in failure output
        for section_name, section_content in report.sections:
            if section_content.strip():
                details += "\n{:-^72}\n".format(f" {section_name} ")
                details += section_content + "\n"

        notes.details = details

    else:
        raise RuntimeError("pytest_runtest_makereport received unknown test status")

    return report


@pytest.hookimpl(wrapper=True)
def pytest_runtest_protocol(item: pytest.Item, nextitem: pytest.Item | None):
    """
    Reports the TAP result for this test item using our gathered TestNotes.
    """
    result = yield

    assert notes_key in item.stash, "pgtap didn't annotate a test item?"
    notes = item.stash[notes_key]

    if notes.failed:
        # notes.failed implies the makereport hook populated notes.details.
        assert notes.details is not None
        tap.fail(item.nodeid, notes.details)
    elif notes.skipped:
        assert notes.skip_reason is not None
        tap.skip(item.nodeid, notes.skip_reason)
    else:
        tap.ok(item.nodeid)

    return result
