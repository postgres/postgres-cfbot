# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the portable threading API in port/pg_threads.h.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

command_ok([ 'test_pg_threads' ], 'test_pg_threads');

done_testing();
