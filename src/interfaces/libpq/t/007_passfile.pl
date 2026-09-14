# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Utils;
use Test::More;

# Test PQpassfileLookup(), via libpq_testclient --passfile.  The lookup is
# purely client-side, so no server is involved.  An argument of "-" is
# passed to the function as NULL, and an argument of "=" as an empty
# string (an empty argv element is not portable).

my $td = PostgreSQL::Test::Utils::tempdir;
my $passfile = "$td/pgpass";

delete $ENV{PGPASSFILE};

# The compiled-in defaults that the lookup falls back to.
my ($defaults) = run_command([ 'libpq_testclient', '--passfile-defaults' ]);
$defaults =~ s/\r//g;
my ($defport, $socketdir) = split /\n/, $defaults;

append_to_file($passfile, <<'EOF');
# a comment line
server.example.com:5432:proddb:diego:secret1
server.example.com:5433:*:diego:secret2
localhost:*:mydb:me:localpw
special.example.com:5432:db\:colon:us\\er:pa\\ss\:word
server.example.com:5432:proddb:diego:shadowed
EOF
append_to_file($passfile,
	"defport.example.com:$defport:defdb:defuser:defportpw\n");
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
	[ 'libpq_testclient', '--passfile', $passfile, '=', '-', 'mydb', 'me' ]);
is($out, 'localpw', 'empty hostname matches a localhost entry');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'defport.example.com', '-', 'defdb', 'defuser'
	]);
is($out, 'defportpw', 'NULL port matches an entry for the default port');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'defport.example.com', '=', 'defdb', 'defuser'
	]);
is($out, 'defportpw', 'empty port matches an entry for the default port');

SKIP:
{
	skip 'no default Unix-socket directory on this platform', 1
	  unless defined $socketdir && $socketdir =~ m{^/};

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', $passfile,
			$socketdir, '-', 'mydb', 'me'
		]);
	is($out, 'localpw',
		'default socket directory matches a localhost entry');
}

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'special.example.com', '5432', 'db:colon', 'us\\er'
	]);
is($out, 'pa\\ss:word', 'escaped characters are matched and de-escaped');

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
		'server.example.com', '5432', '=', 'diego'
	]);
is($err, 'no password found', 'empty dbname returns no password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', 'proddb', '-'
	]);
is($err, 'no password found', 'NULL username returns no password');

($out, $err) = run_command(
	[
		'libpq_testclient', '--passfile', $passfile,
		'server.example.com', '5432', 'proddb', '='
	]);
is($err, 'no password found', 'empty username returns no password');

# A NULL or empty passfile falls back to the PGPASSFILE environment
# variable.
{
	local $ENV{PGPASSFILE} = $passfile;

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', '-',
			'server.example.com', '5432', 'proddb', 'diego'
		]);
	is($out, 'secret1', 'NULL passfile falls back to PGPASSFILE');

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', '=',
			'server.example.com', '5432', 'proddb', 'diego'
		]);
	is($out, 'secret1', 'empty passfile falls back to PGPASSFILE');
}

SKIP:
{
	skip 'default password file location cannot be redirected on Windows', 1
	  if $windows_os;

	# Without PGPASSFILE, the lookup falls back to ~/.pgpass.
	my $homedir = PostgreSQL::Test::Utils::tempdir;
	my $homepassfile = "$homedir/.pgpass";

	append_to_file($homepassfile,
		"home.example.com:5432:homedb:homeuser:homepw\n");
	chmod 0600, $homepassfile or die "chmod: $!";

	local $ENV{HOME} = $homedir;

	($out, $err) = run_command(
		[
			'libpq_testclient', '--passfile', '-',
			'home.example.com', '5432', 'homedb', 'homeuser'
		]);
	is($out, 'homepw', 'NULL passfile falls back to ~/.pgpass');
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
