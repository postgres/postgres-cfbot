# Copyright (c) 2026, PostgreSQL Global Development Group

# Tablespace options defined by an extension

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
allow_in_place_tablespaces = on
shared_preload_libraries = 'test_tablespace_options'
test_tablespace_options.bad_option_name = ''
});
$node->start;

my ($ret, $stdout, $stderr);

sub spcoptions
{
	my $name = shift;
	return $node->safe_psql('postgres',
		"SELECT spcoptions FROM pg_tablespace WHERE spcname = '$name'");
}

# Qualified names work without quotes, next to built-in options
$node->safe_psql('postgres',
	"CREATE TABLESPACE regress_ts1 LOCATION '' WITH (test_tablespace_options.level = 3, test_tablespace_options.mode = safe, random_page_cost = 2)"
);
is( spcoptions('regress_ts1'),
	'{test_tablespace_options.level=3,test_tablespace_options.mode=safe,random_page_cost=2}',
	'CREATE TABLESPACE with extension options');

$node->safe_psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (\"test_tablespace_options.flag\" = true, test_tablespace_options.level = 4)"
);
is( spcoptions('regress_ts1'),
	'{test_tablespace_options.mode=safe,random_page_cost=2,test_tablespace_options.flag=true,test_tablespace_options.level=4}',
	'ALTER TABLESPACE SET with quoted and unquoted names');

$node->safe_psql('postgres',
	"ALTER TABLESPACE regress_ts1 RESET (test_tablespace_options.flag)");
is( spcoptions('regress_ts1'),
	'{test_tablespace_options.mode=safe,random_page_cost=2,test_tablespace_options.level=4}',
	'ALTER TABLESPACE RESET with an unquoted name');

# Values are validated
($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (test_tablespace_options.level = 11)");
like(
	$stderr,
	qr/value 11 out of bounds for option "test_tablespace_options.level"/,
	'out of range value is rejected');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (test_tablespace_options.mode = slow)");
like(
	$stderr,
	qr/invalid value for enum option "test_tablespace_options.mode": slow/,
	'invalid enum value is rejected');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (test_tablespace_options.nonexistent = 1)"
);
like(
	$stderr,
	qr/unrecognized parameter "test_tablespace_options.nonexistent"/,
	'unknown qualified name is rejected');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (other.level = 1)");
like(
	$stderr,
	qr/unrecognized parameter "other.level"/,
	'unknown prefix is rejected');

# Values that need quoting survive pg_dumpall
$node->safe_psql('postgres',
	"CREATE TABLESPACE regress_ts2 LOCATION '' WITH (test_tablespace_options.label = 'a,b''); SELECT 1; --')"
);
my $before = $node->safe_psql('postgres',
	"SELECT spcname, spcoptions FROM pg_tablespace WHERE spcname LIKE 'regress_ts%' ORDER BY 1"
);

my $dumpfile = $node->basedir . '/tablespaces.sql';
$node->command_ok(
	[
		'pg_dumpall', '--tablespaces-only',
		'--file' => $dumpfile,
		'--port' => $node->port,
	],
	'pg_dumpall of tablespaces');

$node->safe_psql('postgres', 'DROP TABLESPACE regress_ts1');
$node->safe_psql('postgres', 'DROP TABLESPACE regress_ts2');
$node->command_ok(
	[
		'psql', '--no-psqlrc',
		'--set' => 'ON_ERROR_STOP=1',
		'--file' => $dumpfile,
		'--port' => $node->port,
		'postgres',
	],
	'restore of tablespaces');
is( $node->safe_psql('postgres',
		"SELECT spcname, spcoptions FROM pg_tablespace WHERE spcname LIKE 'regress_ts%' ORDER BY 1"
	),
	$before,
	'tablespace options survive dump and restore');

# Without the extension, its options have no effect and block other changes
$node->safe_psql('postgres',
	"CREATE TABLE regress_tab (i int) TABLESPACE regress_ts1");
$node->adjust_conf('postgresql.conf', 'shared_preload_libraries', "''");
$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM regress_tab'),
	'0', 'table in tablespace with unknown options is usable');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 SET (seq_page_cost = 2)");
like(
	$stderr,
	qr/unrecognized parameter "test_tablespace_options\.\w+"/,
	'SET fails while unknown options are set');

($ret, $stdout, $stderr) = $node->psql('postgres',
	"ALTER TABLESPACE regress_ts1 RESET (test_tablespace_options.level)");
like(
	$stderr,
	qr/unrecognized parameter "test_tablespace_options.mode"/,
	'RESET of only some unknown options fails');

$node->safe_psql('postgres',
	"ALTER TABLESPACE regress_ts1 RESET (test_tablespace_options.level, test_tablespace_options.mode)"
);
is(spcoptions('regress_ts1'), '{random_page_cost=2}',
	'RESET of all unknown options works');

# Loading the library later is refused
($ret, $stdout, $stderr) =
  $node->psql('postgres', "LOAD 'test_tablespace_options'");
like(
	$stderr,
	qr/FATAL:  could not register tablespace option "test_tablespace_options.level"/,
	'LOAD outside shared_preload_libraries is refused');
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'server survives the refused LOAD');

# Invalid names stop the server from starting
$node->adjust_conf('postgresql.conf', 'shared_preload_libraries',
	"'test_tablespace_options'");
$node->stop;

my @bad_names = (
	[ 'nodot', qr/invalid tablespace option name "nodot"/ ],
	[ 'bad-char.option', qr/invalid tablespace option name "bad-char.option"/ ],
	[
		'a' x 32 . '.' . 'b' x 32,
		qr/tablespace option name "a{32}\.b{32}" is too long/
	]);

foreach my $case (@bad_names)
{
	my ($name, $error) = @$case;
	my $log_offset = -s $node->logfile;

	$node->adjust_conf('postgresql.conf',
		'test_tablespace_options.bad_option_name', "'$name'");
	ok(!$node->start(fail_ok => 1), "server does not start with option \"$name\"");
	ok($node->log_contains($error, $log_offset),
		"error reported for option \"$name\"");
}

$node->adjust_conf('postgresql.conf',
	'test_tablespace_options.bad_option_name', "''");
$node->start;
$node->stop;

done_testing();
