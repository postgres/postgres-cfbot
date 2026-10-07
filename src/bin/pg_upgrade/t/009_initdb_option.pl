# Copyright (c) 2026, PostgreSQL Global Development Group

# Test pg_upgrade's --initdb and --initdb-options options.

use strict;
use warnings FATAL => 'all';

use Config;
use Cwd qw(abs_path);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Use nondefault settings to verify that --initdb carries them over to the
# new cluster.
my $oldnode = PostgreSQL::Test::Cluster->new('old_node');
$oldnode->init(
	extra => [
		'--allow-group-access',
		'--no-data-checksums',
		'--wal-segsize' => '2',
		'--locale' => 'C',
	]);
$oldnode->start;
$oldnode->safe_psql('postgres',
		"CREATE TABLE t (id int primary key, note text); "
	  . "INSERT INTO t SELECT g, 'row ' || g FROM generate_series(1, 100) g; "
	  . "CREATE DATABASE extra_db;");
my $rows_before = $oldnode->safe_psql('postgres', 'SELECT count(*) FROM t');
is($rows_before, '100', 'old cluster has expected rows before upgrade');

# Capture the old settings for the post-upgrade comparison.
my %setting_queries = (
	data_checksums => 'SHOW data_checksums',
	wal_segment_size => 'SHOW wal_segment_size',
	encoding =>
	  "SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = 'template0'",
	collation =>
	  "SELECT datcollate FROM pg_database WHERE datname = 'template0'",
	ctype => "SELECT datctype FROM pg_database WHERE datname = 'template0'",
	provider =>
	  "SELECT datlocprovider FROM pg_database WHERE datname = 'template0'",);
my %old_settings;
for my $setting (sort keys %setting_queries)
{
	$old_settings{$setting} =
	  $oldnode->safe_psql('postgres', $setting_queries{$setting});
}
$oldnode->stop;

# Leave the new cluster uninitialized so pg_upgrade --initdb creates it.
my $newnode = PostgreSQL::Test::Cluster->new('new_node');

my $oldbindir = $oldnode->config_data('--bindir');
my $newbindir = $newnode->config_data('--bindir');

sub upgrade_command
{
	my ($old, $new, @extra) = @_;
	return [
		'pg_upgrade', '--no-sync', '--initdb',
		'--old-datadir' => $old->data_dir,
		'--new-datadir' => $new->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $new->host,
		'--old-port' => $old->port,
		'--new-port' => $new->port,
		@extra,
	];
}

# Configure the connection settings normally written by init(), without
# changing initdb's pg_hba.conf.
sub start_new_cluster
{
	my ($node) = @_;
	my $listen = $PostgreSQL::Test::Cluster::use_tcp ? $node->host : '';
	my $socketdir = $PostgreSQL::Test::Cluster::use_tcp ? '' : $node->host;
	$node->append_conf('postgresql.conf',
			'port = '
		  . $node->port
		  . "\nlisten_addresses = '$listen'\nunix_socket_directories = '$socketdir'"
	);
	$node->start;
}

ok(!-d $newnode->data_dir,
	'new cluster data directory does not exist before --initdb');

# Run pg_upgrade --initdb from a writable directory for its
# pg_upgrade_output.d logs.
# Pass the server-only option -F through -O to verify that it reaches
# the new server without being passed to initdb.
# Repeated --initdb-options must preserve argument order and quoted values in
# the configuration used after the upgrade.
chdir ${PostgreSQL::Test::Utils::tmp_check};

command_ok(
	upgrade_command(
		$oldnode, $newnode,
		'--new-options' => '-F',
		'--initdb-options' =>
		  '-c huge_pages=try -c custom.initdb_first=first',
		'--initdb-options' =>
		  q{-c huge_pages=off --set=custom.initdb_first=last -c "custom.initdb_text=space, apostrophe's $libdir" -c custom.initdb_path=C:\new\path -c custom.initdb_empty=""},
		'--initdb-options' =>
		  q{-c 'custom.initdb_single=single quoted $libdir' -c "custom.initdb_escaped=quote\" slash\\\\" -c custom.initdb_join='joined 'pieces -c custom.initdb_space=escaped\ space},
	),
	'run of pg_upgrade --initdb with -O creates and upgrades the new cluster'
);

# Check before the test harness starts the target and overwrites this file.
like(slurp_file($newnode->data_dir . '/postmaster.opts'),
	qr/(?:^|\s)"-F"(?:\s|$)/, '-O option reached the target postmaster');

ok(-f $newnode->data_dir . '/PG_VERSION',
	'new cluster data directory created by --initdb');

# Group access requested for the old cluster must be carried over to the new
# cluster.  Otherwise programs that read PGDATA as a group member lose access
# after the upgrade.
SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os || $Config::Config{osname} eq 'cygwin');

	my $old_mode = (stat($oldnode->data_dir))[2] & 0777;
	my $new_mode = (stat($newnode->data_dir))[2] & 0777;
	is($new_mode, $old_mode, 'new cluster preserves PGDATA group access');
}

# --initdb logs use pg_upgrade_output.d with the normal cleanup and --retain
# handling.  Check that no separate <pgdata>.initdb_log directory remains
# beside the new data directory.
ok(!-d $newnode->data_dir . '.initdb_log',
	'--initdb does not leave a sibling log directory');

start_new_cluster($newnode);

my %expected_settings = (
	huge_pages => 'off',
	'custom.initdb_first' => 'last',
	'custom.initdb_text' => q{space, apostrophe's $libdir},
	'custom.initdb_path' => q{C:\new\path},
	'custom.initdb_empty' => '',
	'custom.initdb_single' => q{single quoted $libdir},
	'custom.initdb_escaped' => "quote\" slash\\",
	'custom.initdb_join' => 'joined pieces',
	'custom.initdb_space' => 'escaped space',);
for my $setting (sort keys %expected_settings)
{
	is( $newnode->safe_psql('postgres', "SHOW $setting"),
		$expected_settings{$setting},
		"$setting survives initialization and upgrade");
}

my $rows_after = $newnode->safe_psql('postgres', 'SELECT count(*) FROM t');
is($rows_after, '100', 'user data survived --initdb upgrade');

my $has_extra = $newnode->safe_psql('postgres',
	"SELECT count(*) FROM pg_database WHERE datname = 'extra_db'");
is($has_extra, '1', 'user database carried over by --initdb upgrade');

# The new cluster must match the old checksum, WAL segment, encoding, and
# locale settings.
for my $setting (sort keys %setting_queries)
{
	is($newnode->safe_psql('postgres', $setting_queries{$setting}),
		$old_settings{$setting}, "$setting propagated by --initdb");
}

$newnode->stop;

# Check that pg_upgrade rejects an existing cluster before invoking initdb.
# The error is reported on stdout.
command_checks_all(upgrade_command($oldnode, $newnode),
	1, [qr/is not empty/], [qr/^$/],
	'--initdb refuses to overwrite an existing cluster');

# Reject overlap with the output directory before creating logs or starting
# either server, for both a real upgrade and --check.
my $original_cwd = abs_path('.');
for my $check (0, 1)
{
	for my $target ('.', 'pg_upgrade_output.d', 'pg_upgrade_output.d/new')
	{
		my $overlap_cwd = PostgreSQL::Test::Utils::tempdir;
		chdir $overlap_cwd or die "could not change to $overlap_cwd: $!";
		my $mode = $check ? '--check --initdb' : '--initdb';
		command_checks_all(
			upgrade_command(
				$oldnode, $newnode,
				'--new-datadir' => $target,
				$check ? '--check' : ()),
			1,
			[qr/overlaps output directory/],
			[qr/^$/],
			"$mode rejects target $target overlapping its output directory");
		is_deeply([ grep { $_ ne '.' && $_ ne '..' } slurp_dir('.') ],
			[], "$mode overlap failure leaves the working directory empty");
		chdir $original_cwd or die "could not change to $original_cwd: $!";
	}
}

# Validate the new binaries before initialization.
my $empty_bindir = PostgreSQL::Test::Utils::tempdir;
command_checks_all(
	upgrade_command(
		$oldnode, $newnode,
		'--new-datadir' => $newnode->data_dir . '_nonexistent',
		'--new-bindir' => $empty_bindir),
	1,
	[qr/check for .*postgres.* failed/],
	[qr/^$/],
	'--initdb fails early when the new binaries are missing');

# --check --initdb initializes the new cluster and runs compatibility checks
# on both clusters.
my $checked_target = $newnode->data_dir . '_check';
command_checks_all(
	upgrade_command(
		$oldnode, $newnode,
		'--new-datadir' => $checked_target,
		'--check'),
	0,
	[qr/Clusters are compatible/],
	[qr/^$/],
	'--check --initdb initializes the target and checks both clusters');

ok(-f "$checked_target/PG_VERSION",
	'successful check retains the new cluster');
ok( !-f "$checked_target/postmaster.pid",
	'successful check stops the new server');

# Without -B, --initdb must derive the new bindir from the original argv[0],
# even when that directory is not in PATH.
SKIP:
{
	skip "restricted PATH test is not portable to Windows", 3 if $windows_os;

	local %ENV = %ENV;
	$ENV{PATH} = '/usr/bin:/bin';
	command_checks_all(
		[
			abs_path("$newbindir/pg_upgrade"), '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir . '_no_new_bindir',
			'--old-bindir' => $oldbindir,
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $newnode->port,
			'--initdb',
			'--check',
		],
		0,
		[qr/Clusters are compatible/],
		[qr/^$/],
		'--initdb derives the new bindir from an absolute argv[0]');
}

# --check --initdb must accept a running old server and leave it running.
# Use a different port for the new server.
SKIP:
{
	skip "Timing issues with live server detection on Windows", 4
	  if $windows_os;

	$oldnode->start;
	command_checks_all(
		upgrade_command(
			$oldnode, $newnode,
			'--new-datadir' => $newnode->data_dir . '_live_check',
			'--check'),
		0,
		[qr/Clusters are compatible/],
		[qr/^$/],
		'--check --initdb accepts a live source');
	is($oldnode->safe_psql('postgres', 'SELECT 1'),
		'1', 'live source remains running after --check --initdb');
	$oldnode->stop;
}

# Both upgrade and --check must reject regproc columns in user tables and
# retain the initialized new cluster.
$oldnode->start;
$oldnode->safe_psql('postgres', 'CREATE TABLE bad (c regproc)');
$oldnode->stop;

for my $check (0, 1)
{
	my $target = $newnode->data_dir . "_incompatible_$check";
	my $mode = $check ? '--check --initdb' : '--initdb';
	command_checks_all(
		upgrade_command(
			$oldnode, $newnode,
			'--new-datadir' => $target,
			$check ? '--check' : ()),
		1,
		[qr/failed check: Checking for reg\* data types in user tables/],
		[qr/^$/],
		"$mode rejects an incompatible old cluster");
	ok(-f "$target/PG_VERSION", "failed $mode retains the new cluster");
}

# Require --initdb even when --initdb-options is empty.
# Reject invalid quoting and line breaks before examining either cluster.
command_checks_all(
	[ 'pg_upgrade', '--initdb-options=' ],
	1,
	[qr/--initdb-options requires --initdb/],
	[qr/^$/],
	'--initdb-options requires --initdb even for an empty argument');

for my $case (
	[
		q{-c 'huge_pages=off}, qr/unterminated quote/,
		'unclosed single quote'
	],
	[
		q{-c "huge_pages=off}, qr/unterminated quote/,
		'unclosed double quote'
	],
	[ "-c huge_pages=off\\", qr/trailing backslash/, 'trailing escape' ],
	[ "-c huge_pages=off\n", qr/newline or carriage return/, 'newline' ])
{
	command_checks_all(
		[ 'pg_upgrade', '--initdb', "--initdb-options=$case->[0]" ],
		1, [ $case->[1] ],
		[qr/^$/], "reject $case->[2]");
}

# Reject options that override pg_upgrade's settings or skip initialization.
for my $options (
	'-D/another/datadir',
	'--username=another_user',
	'--wal-segsize=32',
	'--no-data-checksums',
	'--encoding=UTF8',
	'--locale=C',
	'--locale-provider=builtin',
	'--sync-only',
	'--help',
	'-c data_directory=/another/datadir',
	'--set=CONFIG_FILE=/another/config',
	'-c Hba-File=/another/hba',
	'-cident_file=/another/ident')
{
	command_checks_all(
		[ 'pg_upgrade', '--initdb', "--initdb-options=$options" ], 1,
		[qr/cannot be used with --initdb/], [qr/^$/],
		"reject managed or incompatible option: $options");
}

sub option_quote
{
	my ($value) = @_;
	$value =~ s/'/'\\''/g;
	return "'$value'";
}

# test_slru rejects LOAD unless preloaded.  The new cluster must preload it
# for pg_upgrade's loadable-library check to pass.
my $preload_old = PostgreSQL::Test::Cluster->new('preload_old');
$preload_old->init;
$preload_old->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_slru'");
$preload_old->start;
$preload_old->safe_psql('postgres', 'CREATE EXTENSION test_slru');
my $quoted_superuser =
  $preload_old->safe_psql('postgres', 'SELECT quote_ident(current_user)');
# This password is used only by the temporary test clusters.
my $password = 'initdb-options-test-password';
$preload_old->safe_psql('postgres',
	"ALTER ROLE $quoted_superuser PASSWORD '$password'");
$preload_old->stop;

my $preload_missing = PostgreSQL::Test::Cluster->new('preload_missing');
my $preload_missing_wal = $preload_missing->basedir . '/wal';
command_checks_all(
	upgrade_command(
		$preload_old,
		$preload_missing,
		'--check',
		'--initdb-options' => '--waldir=' . option_quote($preload_missing_wal)
	),
	1,
	[qr/references loadable libraries that are missing/],
	[qr/^$/],
	'--check --initdb checks required libraries in the new cluster');
ok( -f $preload_missing->data_dir . '/PG_VERSION',
	'failed preload check retains the new cluster');
ok(-d $preload_missing_wal, 'failed preload check retains the WAL directory');
ok( !-f $preload_missing->data_dir . '/postmaster.pid',
	'failed preload check stops the new server');

my $credential_dir = PostgreSQL::Test::Utils::tempdir;
my $pwfile = "$credential_dir/initdb.pw";
append_to_file($pwfile, "$password\n");
chmod(0600, $pwfile) or die "could not protect $pwfile: $!";
my $pgpass = "$credential_dir/pgpass";
append_to_file($pgpass, "*:*:*:*:$password\n");
chmod(0600, $pgpass) or die "could not protect $pgpass: $!";

my $auth_options =
  q{-c shared_preload_libraries=$libdir/test_slru --auth-local=scram-sha-256 --auth-host=scram-sha-256 --pwfile="}
  . $pwfile . '"';

{
	local $ENV{PGPASSFILE} = "$credential_dir/no-password-file";
	delete local $ENV{PGPASSWORD};
	my $auth_missing = PostgreSQL::Test::Cluster->new('auth_missing');
	command_checks_all(
		upgrade_command(
			$preload_old, $auth_missing,
			'--initdb-options' => $auth_options),
		1,
		[qr/fe_sendauth: no password supplied/],
		[qr/^$/],
		'password authentication is not weakened when credentials are missing'
	);
	ok( -f $auth_missing->data_dir . '/PG_VERSION',
		'authentication failure retains the new cluster');
	ok( !-f $auth_missing->data_dir . '/postmaster.pid',
		'authentication failure stops the new server');
}

{
	local $ENV{PGPASSFILE} = $pgpass;
	my $preload_new = PostgreSQL::Test::Cluster->new('preload_new');
	my $wal = $preload_new->basedir . '/wal with spaces';
	command_ok(
		upgrade_command(
			$preload_old,
			$preload_new,
			'--initdb-options' => $auth_options
			  . ' --waldir='
			  . option_quote($wal)),
		'initdb options support a preload-only extension and password authentication'
	);
	ok(-d $wal, 'initdb uses the requested WAL directory');

	my $hba = slurp_file($preload_new->data_dir . '/pg_hba.conf');
	my @rules = grep { /\S/ && !/^\s*#/ } split(/\n/, $hba);
	ok(!(grep { !/\bscram-sha-256\s*$/ } @rules),
		'all generated authentication rules keep the requested method');
  SKIP:
	{
		skip 'unix-style permissions not supported on Windows', 1
		  if ($windows_os || $Config::Config{osname} eq 'cygwin');
		is((stat($preload_new->data_dir))[2] & 0777,
			0700, 'owner-only data directory mode is inherited');
	}

	start_new_cluster($preload_new);
	is( $preload_new->safe_psql('postgres', 'SHOW shared_preload_libraries'),
		q{$libdir/test_slru},
		'preload setting preserves literal $libdir');
	$preload_new->safe_psql('postgres',
		"SELECT test_slru_page_write(0, 'upgraded')");
	is( $preload_new->safe_psql('postgres', 'SELECT test_slru_page_read(0)'),
		'upgraded',
		'restored preload-only extension is usable');
	$preload_new->stop;
}

# Link and swap retain old file modes, so a group-access request must not
# turn an owner-only cluster into a partly group-readable cluster.
SKIP:
{
	skip 'Unix permissions are not supported on this platform', 1
	  if $windows_os || $Config::Config{osname} eq 'cygwin';

	subtest 'group access with link and swap' => sub {
		for my $mode ('--link', '--swap')
		{
			for my $check (0, 1)
			{
				my $new = PostgreSQL::Test::Cluster->new(
					substr($mode, 2) . "_group_rejected_$check");
				command_checks_all(
					upgrade_command(
						$preload_old, $new, $mode,
						'--initdb-options' => $check
						? '-dg'
						: '--allow-group-access',
						$check ? '--check' : ()),
					1,
					[qr/cannot enable group access with \Q$mode\E/],
					[qr/^$/],
					"$mode rejects group access with check=$check");
				ok(!-d $new->data_dir,
					'permission mismatch leaves no new cluster');
			}

			my $name = substr($mode, 2);
			my $old = PostgreSQL::Test::Cluster->new("old_$name");
			my $new = PostgreSQL::Test::Cluster->new("new_$name");
			$old->init(extra => ['--allow-group-access']);
			$old->start;
			$old->safe_psql('postgres',
				'CREATE TABLE permissions (id integer); INSERT INTO permissions VALUES (1)'
			);
			my $relpath = $old->safe_psql('postgres',
				"SELECT pg_relation_filepath('permissions')");
			$old->stop;
			command_ok(
				upgrade_command(
					$old, $new,
					$mode, '--initdb-options' => '--allow-group-access'),
				"$mode accepts matching group access settings");
			is((stat($new->data_dir))[2] & 0777,
				0750, "$mode preserves data directory permissions");
			is((stat($new->data_dir . "/$relpath"))[2] & 0777,
				0640, "$mode preserves relation permissions");
		}
		done_testing();
	};
}

done_testing();
