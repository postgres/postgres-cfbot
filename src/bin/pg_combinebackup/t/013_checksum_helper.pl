# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Utils;
use Test::More;

command_exit_is(
	['test_checksum_helper'],
	0,
	'checksum contexts are cleaned up after cryptohash failures');

done_testing();
