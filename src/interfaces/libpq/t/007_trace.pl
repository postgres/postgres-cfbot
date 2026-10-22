# Copyright (c) 2026, PostgreSQL Global Development Group

# Test libpq's protocol message tracing with messages whose contents don't
# fit in their length.  The tracer must stop at the end of the message, and
# report it, instead of reading past it.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run;

# Each test is the direction (F or B), the message type, its contents in hex,
# and the expected trace output.  libpq_trace_regress fills in the length.
my @tests = (
	# Well-formed messages
	[ 'B', 'D', '0002 00000003 616263 ffffffff', qq{B\t17\tDataRow\t 2 3 'abc' -1} ],
	[ 'B', 'N', '53 4e4f5449434500 00', qq{B\t13\tNoticeResponse\t S "NOTICE" \\x00} ],
	[ 'B', 'd', '68656c6c6f0a', qq{B\t10\tCopyData\t 'hello\\x0a'} ],
	[ 'B', 'K', '00000001 0203', qq{B\t10\tBackendKeyData\t 1 '\\x02\\x03'} ],
	[ 'F', 'C', '53 666f6f00', qq{F\t9\tClose\t S "foo"} ],
	[
		'B', 'R', '0000000a 534352414d2d5348412d32353600 00',
		qq{B\t23\tAuthenticationSASL\t "SCRAM-SHA-256" ""}
	],
	[
		'B', 'v', '00030000 00000001 5f707100',
		qq{B\t16\tNegotiateProtocolVersion\t 196608 1 "_pq"}
	],
	[ 'B', 'W', '00 0001 0000', qq{B\t9\tCopyBothResponse\t \\x00 1 0} ],

	# A negative field length other than -1 is a NULL too, as libpq reads it
	[ 'B', 'D', '0002 fffffffe 00000001 62', qq{B\t15\tDataRow\t 2 -2 1 'b'} ],

	# Contents left over at the end are still reported
	[
		'B', 'Z', '4900',
		qq{B\t6\tReadyForQuery\t I\nmismatched message length: consumed 5, expected 6}
	],

	# Fields that run past the end of the message
	[
		'B', 'D', '0001 000003e8 616263',
		qq{B\t13\tDataRow\t 1 1000 'abc'\nmismatched message length: contents exceed length 13}
	],
	[
		'B', 'D', '0001 7fffffff 61',
		qq{B\t11\tDataRow\t 1 2147483647 'a'\nmismatched message length: contents exceed length 11}
	],
	[
		'B', 'V', '00000004 6162',
		qq{B\t10\tFunctionCallResponse\t 4 'ab'\nmismatched message length: contents exceed length 10}
	],
	[
		'B', 'V', 'fffffffb',
		qq{B\t8\tFunctionCallResponse\t -5 ''\nmismatched message length: contents exceed length 8}
	],

	# A frontend message type is printed as such, even coming from the server
	[
		'B', 'B', '00 00 0001 0000 0001 000000ff 41',
		qq{B\t17\tBind\t "" "" 1 0 1 255 'A'\nmismatched message length: contents exceed length 17}
	],

	# Counts of more fields than the message holds
	[
		'B', 'D', '0003 00000001 61',
		qq{B\t11\tDataRow\t 3 1 'a'\nmismatched message length: contents exceed length 11}
	],
	[
		'B', 'T', '0001 6100 00000000 0000',
		qq{B\t14\tRowDescription\t 1 "a" 0 0\nmismatched message length: contents exceed length 14}
	],
	[
		'B', 't', '0002 00000017',
		qq{B\t10\tParameterDescription\t 2 23\nmismatched message length: contents exceed length 10}
	],
	[
		'B', 'G', '00 0003 0000',
		qq{B\t9\tCopyInResponse\t \\x00 3 0\nmismatched message length: contents exceed length 9}
	],
	[
		'B', 'v', '00030000 7fffffff 5f707100',
		qq{B\t16\tNegotiateProtocolVersion\t 196608 2147483647 "_pq"\nmismatched message length: contents exceed length 16}
	],
	[
		'B', 'W', '00 0003 0000',
		qq{B\t9\tCopyBothResponse\t \\x00 3 0\nmismatched message length: contents exceed length 9}
	],
	[
		'B', 'W', '00 0001 00',
		qq{B\t8\tCopyBothResponse\t \\x00 1\nmismatched message length: contents exceed length 8}
	],

	# Strings and lists without their terminating zero byte
	[
		'B', 'C', '53454c454354',
		qq{B\t10\tCommandComplete\t "SELECT"\nmismatched message length: contents exceed length 10}
	],
	[
		'B', 'S', '6100 62',
		qq{B\t7\tParameterStatus\t "a" "b"\nmismatched message length: contents exceed length 7}
	],
	[
		'B', 'A', '00000001 6300',
		qq{B\t10\tNotificationResponse\t 1 "c"\nmismatched message length: contents exceed length 10}
	],
	[
		'B', 'E', '53 4552524f5200',
		qq{B\t11\tErrorResponse\t S "ERROR"\nmismatched message length: contents exceed length 11}
	],
	[
		'B', 'R', '0000000a 534352414d2d5348412d32353600',
		qq{B\t22\tAuthenticationSASL\t "SCRAM-SHA-256"\nmismatched message length: contents exceed length 22}
	],
	[
		'B', 'R', '',
		qq{B\t4\t\nmismatched message length: contents exceed length 4}
	]);

foreach my $test (@tests)
{
	my ($direction, $type, $data, $expected) = @$test;
	my $name = "pqTraceOutputMessage: $direction '$type' $data";

	my ($stdout, $stderr);
	my $result = IPC::Run::run [ 'libpq_trace_regress', $direction, $type, $data ],
	  '>' => \$stdout,
	  '2>' => \$stderr;
	chomp($stdout);

	ok($result, "$name: exit status");
	is($stdout, $expected, "$name: trace");
	is($stderr, '', "$name: no errors");
}

done_testing();
