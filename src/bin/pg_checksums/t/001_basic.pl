
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('pg_checksums');
program_version_ok('pg_checksums');
program_options_handling_ok('pg_checksums');

command_fails_like(
	[ 'pg_checksums', '--filenode' => '0' ],
	qr@\Qpg_checksums: error: -f/--filenode must be in range 1..2147483647\E@,
	'pg_checksums: --filenode must be in range');

done_testing();
