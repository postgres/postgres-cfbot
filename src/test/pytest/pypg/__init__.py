# Copyright (c) 2025, PostgreSQL Global Development Group

from __future__ import annotations

from ._env import (
    check_pg_config,
    clean_libpq_environment,
    extension_available,
    pg_test_timeout_default,
    require_extension,
    require_injection_points,
    require_test_extras,
    set_test_locale,
    skip_unless_extension,
    skip_unless_injection_points,
    skip_unless_test_extras,
)
from .server import PostgresServer
from .wait import wait_until

# Clear inherited libpq connection environment variables and fix the locale as
# soon as the test framework is imported, before any server is started or
# connection is made.
clean_libpq_environment()
set_test_locale()

__all__ = [
    "PostgresServer",
    "check_pg_config",
    "extension_available",
    "pg_test_timeout_default",
    "require_extension",
    "require_injection_points",
    "require_test_extras",
    "skip_unless_extension",
    "skip_unless_injection_points",
    "skip_unless_test_extras",
    "wait_until",
]
