# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;

my ($stdout, $stderr) = run_command(['test_xlogreader']);

is($stderr, '', 'no error output');
like($stdout, qr/All tests passed/, 'malformed hole geometry is rejected');

done_testing();
