# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Utils;
use Test::More;

# Test PQpassfileLookup(), via libpq_testclient --passfile.  The lookup is
# purely client-side, so no server is involved.  An argument of "-" is
# passed to the function as NULL.

my $td = PostgreSQL::Test::Utils::tempdir;
my $passfile = "$td/pgpass";

delete $ENV{PGPASSFILE};

append_to_file($passfile, <<'EOF');
# a comment line
server.example.com:5432:proddb:diego:secret1
server.example.com:5433:*:diego:secret2
localhost:*:mydb:me:localpw
special.example.com:5432:db\:colon:us\\er:pa\\ss\:word
*:*:*:fallback:fallpw:anything after an unescaped colon is ignored
server.example.com:5432:proddb:diego:shadowed
EOF
chmod 0600, $passfile or die "chmod: $!";

my ($out, $err);

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', 'proddb', 'diego'
	]);
is($out, 'secret1', 'exact match returns the first matching password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5433', 'anydb', 'diego'
	]);
is($out, 'secret2', 'wildcard field matches any value');

($out, $err) = run_command(
	[ 'libpq_testclient', '--passfile', $passfile, '-', '-', 'mydb', 'me' ]);
is($out, 'localpw', 'NULL hostname and port match a localhost entry');

($out, $err) = run_command(
	[ 'libpq_testclient', '--passfile', $passfile, '', '-', 'mydb', 'me' ]);
is($out, 'localpw', 'empty hostname matches a localhost entry');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'special.example.com', '5432', 'db:colon', 'us\\er'
	]);
is($out, 'pa\\ss:word', 'escaped characters are matched and de-escaped');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'anyhost', '1234', 'anydb', 'fallback'
	]);
is($out, 'fallpw', 'password field ends at the first unescaped colon');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', 'otherdb', 'diego'
	]);
is($err, 'no password found', 'no matching line returns no password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', "$td/does_not_exist",
		'server.example.com', '5432', 'proddb', 'diego'
	]);
is($err, 'no password found', 'missing password file returns no password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', '-', 'diego'
	]);
is($err, 'no password found', 'NULL dbname returns no password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', 'proddb', '-'
	]);
is($err, 'no password found', 'NULL username returns no password');

# A NULL passfile falls back to the PGPASSFILE environment variable.
{
	local $ENV{PGPASSFILE} = $passfile;

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', '-',
			'server.example.com', '5432', 'proddb', 'diego'
		]);
	is($out, 'secret1', 'NULL passfile falls back to PGPASSFILE');
}

SKIP:
{
	skip 'password file permissions are not checked on Windows', 2
	  if $windows_os;

	my $passfile_insecure = "$td/pgpass_insecure";
	copy($passfile, $passfile_insecure)
	  or die "could not copy $passfile to $passfile_insecure: $!";
	chmod 0644, $passfile_insecure or die "chmod: $!";

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', $passfile_insecure,
			'server.example.com', '5432', 'proddb', 'diego'
		]);
	like(
		$err,
		qr/has group or world access/,
		'insecure password file draws a warning');
	like($err, qr/no password found/, 'insecure password file is ignored');
}

done_testing();
