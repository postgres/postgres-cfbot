# Copyright (c) 2025, PostgreSQL Global Development Group

from __future__ import annotations

import functools
import logging
import os

import pytest

from .paths import INCLUDEDIR_SERVER, SHAREDIR

logger = logging.getLogger(__name__)


# libpq reads many PG* environment variables as connection defaults, and a
# stray value overrides parameters the framework sets explicitly (e.g. GitHub's
# Windows runners preset PGUSER=postgres for their bundled PostgreSQL, breaking
# every connection). Clear them up front.
_LIBPQ_ENV_VARS = (
    "PGAPPNAME",
    "PGCHANNELBINDING",
    "PGCLIENTENCODING",
    "PGCONNECT_TIMEOUT",
    "PGDATA",
    "PGDATABASE",
    "PGGSSDELEGATION",
    "PGGSSENCMODE",
    "PGGSSLIB",
    "PGHOST",
    "PGHOSTADDR",
    "PGKRBSRVNAME",
    "PGOPTIONS",
    "PGPASSFILE",
    "PGPASSWORD",
    "PGPORT",
    "PGREQUIREPEER",
    "PGREQUIRESSL",
    "PGSERVICE",
    "PGSERVICEFILE",
    "PGSSLCERT",
    "PGSSLCRL",
    "PGSSLCRLDIR",
    "PGSSLKEY",
    "PGSSLMAXPROTOCOLVERSION",
    "PGSSLMINPROTOCOLVERSION",
    "PGSSLMODE",
    "PGSSLROOTCERT",
    "PGSSLSNI",
    "PGTARGETSESSIONATTRS",
    "PGUSER",
    # Not read by libpq, but makes client programs color their messages,
    # which breaks matching on their output.
    "PG_COLOR",
)


def clean_libpq_environment() -> None:
    """Remove inherited libpq connection environment variables (see above)."""
    for var in _LIBPQ_ENV_VARS:
        os.environ.pop(var, None)


def set_test_locale() -> None:
    """Make the programs the tests run report messages in English.

    Tests match on message text, but in a build with translations initdb,
    the server (whose lc_messages initdb takes from the environment) and the
    client programs would translate them according to the user's locale.
    LC_NUMERIC is fixed as well, since it affects how they format numbers.
    """
    os.environ.pop("LANGUAGE", None)
    os.environ.pop("LC_ALL", None)
    os.environ["LC_MESSAGES"] = "C"
    os.environ["LC_NUMERIC"] = "C"


def _test_extra_skip_reason(*keys: str) -> str:
    return "requires {} to be set in PG_TEST_EXTRA".format(", ".join(keys))


def _has_test_extra(key: str) -> bool:
    """
    Returns True if the PG_TEST_EXTRA environment variable contains the given
    key.
    """
    extra = os.getenv("PG_TEST_EXTRA", "")
    return key in extra.split()


def require_test_extras(*keys: str) -> pytest.MarkDecorator:
    """
    A convenience annotation which will skip tests if all of the required keys
    are not present in PG_TEST_EXTRA.

    To skip a particular test function or class:

        @pypg.require_test_extras("ldap")
        def test_some_ldap_feature():
            ...

    To skip an entire module:

        pytestmark = pypg.require_test_extra("ssl", "kerberos")
    """
    return pytest.mark.skipif(
        not all(_has_test_extra(k) for k in keys),
        reason=_test_extra_skip_reason(*keys),
    )


def skip_unless_test_extras(*keys: str) -> None:
    """
    Skip the current test/fixture if any of the required keys are not present
    in PG_TEST_EXTRA. Use this inside fixtures where decorators can't be used.

        @pytest.fixture
        def my_fixture():
            skip_unless_test_extras("ldap")
            ...
    """
    if not all(_has_test_extra(k) for k in keys):
        pytest.skip(_test_extra_skip_reason(*keys))


_INJECTION_POINTS_SKIP_REASON = "injection points not supported by this build"


def extension_available(name: str) -> bool:
    """Return whether extension ``name`` is installed.

    Test-only modules (injection_points, test_* extensions) can be missing,
    e.g. when running against an existing installation. This checks for the
    extension's control file rather than querying ``pg_available_extensions``,
    so it needs no running server and works in a collection-time decorator
    (see ``require_extension()``).
    """
    return (SHAREDIR / "extension" / f"{name}.control").exists()


def require_extension(name: str, reason: str | None = None) -> pytest.MarkDecorator:
    """Skip the decorated test/class/module unless extension ``name`` is
    installed::

        @pypg.require_extension("test_wait_lsn")
        def test_something():
            ...

    or, for an entire module::

        pytestmark = pypg.require_extension("test_wait_lsn")
    """
    return pytest.mark.skipif(
        not extension_available(name),
        reason=reason or f"extension {name} is not installed",
    )


def skip_unless_extension(name: str, reason: str | None = None) -> None:
    """Skip the current test/fixture unless extension ``name`` is installed.
    Use this where decorators can't be used; prefer ``require_extension()``
    otherwise.
    """
    if not extension_available(name):
        pytest.skip(reason or f"extension {name} is not installed")


# The injection_points test module is only installed when the server was built
# with injection point support, so its control file is present exactly when
# the feature is available.
def require_injection_points() -> pytest.MarkDecorator:
    """Skip the decorated test/class/module unless the build supports
    injection points.

        @pypg.require_injection_points()
        def test_some_injection_point():
            ...

    or, for an entire module::

        pytestmark = pypg.require_injection_points()
    """
    return require_extension("injection_points", _INJECTION_POINTS_SKIP_REASON)


def skip_unless_injection_points() -> None:
    """Skip the current test/fixture unless the build supports injection
    points. Use this inside fixtures where decorators can't be used; prefer
    the ``require_injection_points()`` decorator otherwise.
    """
    skip_unless_extension("injection_points", _INJECTION_POINTS_SKIP_REASON)


@functools.cache
def _pg_config_h_lines() -> tuple[str, ...]:
    """Return the lines of the server build's ``pg_config.h``, stripped.

    Read once and cached, since the build under test does not change during a
    session.
    """
    path = INCLUDEDIR_SERVER / "pg_config.h"
    return tuple(line.strip() for line in path.read_text().splitlines())


def check_pg_config(line: str) -> bool:
    """Return whether the server build's ``pg_config.h`` contains a line that
    starts with ``line``.

    Use it to gate tests on build-time feature macros, e.g.::

        if not check_pg_config("#define USE_ICU 1"):
            pytest.skip("ICU not supported by this build")
    """
    return any(candidate.startswith(line) for candidate in _pg_config_h_lines())


def pg_test_timeout_default() -> int:
    """
    Returns the value of the PG_TEST_TIMEOUT_DEFAULT environment variable, in
    seconds, or 180 if one was not provided.
    """
    default = os.getenv("PG_TEST_TIMEOUT_DEFAULT", "")
    if not default:
        return 180

    try:
        return int(default)
    except ValueError as v:
        logger.warning("PG_TEST_TIMEOUT_DEFAULT could not be parsed: " + str(v))
        return 180
