# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the --initdb option of pg_upgrade: pg_upgrade creates the new cluster
# itself via initdb, instead of requiring the user to have run initdb first.

use strict;
use warnings FATAL => 'all';

use Config;
use Cwd            qw(abs_path);
use File::Basename qw(basename);
use File::Copy     qw(copy);
use File::Path     qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize and populate the old cluster.
#
# Use settings that --initdb must carry over to the new cluster: group access,
# disabled data checksums (initdb enables them by default since PG18), a
# non-default WAL segment size, and the C locale.
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

# Record the old cluster's settings so we can compare them after the upgrade.
my $old_checksums = $oldnode->safe_psql('postgres', 'SHOW data_checksums');
my $old_wal_segsize =
  $oldnode->safe_psql('postgres', 'SHOW wal_segment_size');
my $old_encoding = $oldnode->safe_psql('postgres',
	"SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = 'template0'"
);
my $old_collate = $oldnode->safe_psql('postgres',
	"SELECT datcollate FROM pg_database WHERE datname = 'template0'");
my $old_ctype = $oldnode->safe_psql('postgres',
	"SELECT datctype FROM pg_database WHERE datname = 'template0'");
my $old_provider = $oldnode->safe_psql('postgres',
	"SELECT datlocprovider FROM pg_database WHERE datname = 'template0'");
$oldnode->stop;

# Create the new node object but do NOT init() it: pg_upgrade --initdb is
# responsible for creating the data directory.  Only new() runs, which
# allocates the port/host/basedir the framework needs.
my $newnode = PostgreSQL::Test::Cluster->new('new_node');

my $oldbindir = $oldnode->config_data('--bindir');
my $newbindir = $newnode->config_data('--bindir');

# Sanity: the new data directory must not exist yet.
ok(!-d $newnode->data_dir,
	'new cluster data directory does not exist before --initdb');

# Run pg_upgrade --initdb from a writable directory for its
# pg_upgrade_output.d logs.
# Pass the server-only option -F through -O to verify that it reaches
# the new server without being passed to initdb.
chdir ${PostgreSQL::Test::Utils::tmp_check};

command_ok(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
		'--new-options' => '-F',
	],
	'run of pg_upgrade --initdb with -O creates and upgrades the new cluster'
);

# Check before the test harness starts the target and overwrites this file.
like(slurp_file($newnode->data_dir . '/postmaster.opts'),
	qr/(?:^|\s)"-F"(?:\s|$)/, '-O option reached the target postmaster');

# pg_upgrade --initdb should have initialized the new data directory.
# Check that PG_VERSION was created there.
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

# Configure the upgraded node to use its assigned port and host.
# Follow PostgreSQL::Test::Cluster's TCP or Unix-domain socket settings.
my $host = $newnode->host;
$newnode->append_conf('postgresql.conf', "port = " . $newnode->port);
if ($PostgreSQL::Test::Cluster::use_tcp)
{
	$newnode->append_conf('postgresql.conf', "unix_socket_directories = ''");
	$newnode->append_conf('postgresql.conf', "listen_addresses = '$host'");
}
else
{
	$newnode->append_conf('postgresql.conf',
		"unix_socket_directories = '$host'");
	$newnode->append_conf('postgresql.conf', "listen_addresses = ''");
}

$newnode->start;

# Verify the user data survived the upgrade.
my $rows_after = $newnode->safe_psql('postgres', 'SELECT count(*) FROM t');
is($rows_after, '100', 'user data survived --initdb upgrade');

# Verify the extra database carried over too.
my $has_extra = $newnode->safe_psql('postgres',
	"SELECT count(*) FROM pg_database WHERE datname = 'extra_db'");
is($has_extra, '1', 'user database carried over by --initdb upgrade');

# Check the checksum and WAL segment size settings required by
# check_control_data().
my $new_checksums = $newnode->safe_psql('postgres', 'SHOW data_checksums');
is($new_checksums, $old_checksums,
	"data_checksums propagated by --initdb ($new_checksums)");

my $new_wal_segsize =
  $newnode->safe_psql('postgres', 'SHOW wal_segment_size');
is($new_wal_segsize, $old_wal_segsize,
	"wal_segment_size propagated by --initdb ($new_wal_segsize)");

# Check template0's encoding and locale settings.
my $new_encoding = $newnode->safe_psql('postgres',
	"SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = 'template0'"
);
is($new_encoding, $old_encoding,
	"template0 encoding propagated by --initdb ($new_encoding)");

my $new_collate = $newnode->safe_psql('postgres',
	"SELECT datcollate FROM pg_database WHERE datname = 'template0'");
is($new_collate, $old_collate,
	"template0 collation propagated by --initdb ($new_collate)");

my $new_ctype = $newnode->safe_psql('postgres',
	"SELECT datctype FROM pg_database WHERE datname = 'template0'");
is($new_ctype, $old_ctype,
	"template0 ctype propagated by --initdb ($new_ctype)");

my $new_provider = $newnode->safe_psql('postgres',
	"SELECT datlocprovider FROM pg_database WHERE datname = 'template0'");
is($new_provider, $old_provider,
	"template0 locale provider propagated by --initdb ($new_provider)");

$newnode->stop;

# Check that pg_upgrade --initdb rejects an existing target cluster.
# Match its PG_VERSION diagnostic on stdout to distinguish this failure
# from initdb's nonempty-directory error.
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
	],
	1,
	[qr/already contains a database system/],
	[qr/^$/],
	'--initdb refuses to overwrite an existing cluster (PG_VERSION check)');

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
			[
				'pg_upgrade', '--no-sync',
				'--old-datadir' => $oldnode->data_dir,
				'--new-datadir' => $target,
				'--old-bindir' => $oldbindir,
				'--new-bindir' => $newbindir,
				'--initdb',
				$check ? '--check' : (),
			],
			1,
			[qr/overlaps output directory/],
			[qr/^$/],
			"$mode rejects target $target overlapping its output directory");
		is_deeply([ grep { $_ ne '.' && $_ ne '..' } slurp_dir('.') ],
			[], "$mode overlap failure leaves the working directory empty");
		chdir $original_cwd or die "could not change to $original_cwd: $!";
	}
}

# --initdb must fail early with a clear message if initdb is not present in the
# new cluster's bin directory.  Point --new-bindir at an empty directory and use
# a fresh (nonexistent) new data directory so we reach the initdb-present check.
my $empty_bindir = PostgreSQL::Test::Utils::tempdir;
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir . '_nonexistent',
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $empty_bindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
	],
	1,
	[qr/could not find "initdb"/],
	[qr/^$/],
	'--initdb fails early when initdb is missing from the new bindir');

# --check --initdb performs all source-side validation without creating a
# target cluster.
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir . '_dry_run',
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
		'--check',
	],
	0,
	[qr/Source cluster compatibility checks passed/],
	[qr/^$/],
	'--check --initdb runs source-side checks without creating the cluster');

# Verify that --check --initdb didn't create anything.
ok(!-d $newnode->data_dir . '_dry_run',
	'--check --initdb does not create the new cluster directory');
ok( !-d $newnode->data_dir . '_dry_run.initdb_log',
	'--check --initdb does not leave a sibling log directory');

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
		[qr/Source cluster compatibility checks passed/],
		[qr/^$/],
		'--initdb derives the new bindir from an absolute argv[0]');
}

# A live source is valid for --check --initdb, just as it is for ordinary
# --check.  No target server is started, so the ports can be equal.
# pg_upgrade must neither stop nor modify the source server.
SKIP:
{
	skip "Timing issues with live server detection on Windows", 4
	  if $windows_os;

	$oldnode->start;
	command_checks_all(
		[
			'pg_upgrade', '--no-sync',
			'--old-datadir' => $oldnode->data_dir,
			'--new-datadir' => $newnode->data_dir . '_live_check',
			'--old-bindir' => $oldbindir,
			'--new-bindir' => $newbindir,
			'--socketdir' => $newnode->host,
			'--old-port' => $oldnode->port,
			'--new-port' => $oldnode->port,
			'--initdb',
			'--check',
		],
		0,
		[qr/Source cluster compatibility checks passed/],
		[qr/^$/],
		'--check --initdb accepts a live source with equal old and new ports'
	);
	is($oldnode->safe_psql('postgres', 'SELECT 1'),
		'1', 'live source remains running after --check --initdb');
	$oldnode->stop;
}

# Force a failure while the target postmaster is running.  This verifies that
# the exit handlers stop the postmaster before cleaning its data directory.
my $test_library = $ENV{TEST_EXT_LIB}
  or die "could not get the test extension library path";
my $missing_library =
  PostgreSQL::Test::Utils::tempdir() . '/' . basename($test_library);
copy($test_library, $missing_library)
  or die "could not copy $test_library to $missing_library: $!";
my $sql_library = $missing_library =~ s/'/''/gr;

$oldnode->start;
$oldnode->safe_psql('postgres',
		"CREATE FUNCTION missing_upgrade_library() RETURNS void "
	  . "AS '$sql_library', 'test_ext' LANGUAGE C");
$oldnode->stop;
unlink($missing_library) or die "could not remove $missing_library: $!";

# On failure after initdb, preserve an empty directory supplied by the
# operator, including its original mode, but remove everything initdb added.
my $existing_target = $newnode->data_dir . '_existing_empty';
mkdir($existing_target, 0711) or die "could not create $existing_target: $!";
chmod(0711, $existing_target)
  or die "could not set mode on $existing_target: $!";
my $existing_target_mode = (stat($existing_target))[2] & 0777;

command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $existing_target,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
	],
	1,
	[qr/references loadable libraries that are missing/],
	[qr/^$/],
	'failed --initdb stops the target and preserves an existing directory');

ok(-d $existing_target, 'operator-created target directory still exists');
opendir(my $target_dir, $existing_target)
  or die "could not open $existing_target: $!";
my @target_entries = grep { $_ ne '.' && $_ ne '..' } readdir($target_dir);
closedir($target_dir);
is_deeply(\@target_entries, [],
	'operator-created target directory is empty after cleanup');

SKIP:
{
	skip "unix-style permissions not supported on Windows", 1
	  if ($windows_os || $Config::Config{osname} eq 'cygwin');

	my $mode_after_failure = (stat($existing_target))[2] & 0777;
	is($mode_after_failure, $existing_target_mode,
		'operator-created target directory mode is restored');
}

# Starting another server on the target port confirms that the failed
# pg_upgrade did not leave its temporary target postmaster running.
$newnode->start;
is($newnode->safe_psql('postgres', 'SELECT 1'),
	'1', 'target port is free after failed --initdb cleanup');
$newnode->stop;

# Verify that the dry run executes compatibility checks rather than merely
# printing an initdb command.
$oldnode->start;
$oldnode->safe_psql('postgres',
	'DROP FUNCTION missing_upgrade_library(); CREATE TABLE bad (c regproc)');
$oldnode->stop;

my $incompatible_target = $newnode->data_dir . '_incompatible_dry_run';
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $incompatible_target,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
		'--check',
	],
	1,
	[qr/failed check: Checking for reg\* data types in user tables/],
	[qr/^$/],
	'--check --initdb rejects an incompatible source cluster');
ok(!-d $incompatible_target,
	'failed --check --initdb does not create the target directory');

# Conversely, pg_upgrade owns a target directory that did not exist before
# initdb and removes it completely after a later failure.
my $created_target = $newnode->data_dir . '_created_then_failed';
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $created_target,
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
	],
	1,
	[qr/failed check: Checking for reg\* data types in user tables/],
	[qr/^$/],
	'failed --initdb removes a target it created');
ok(!-d $created_target, 'pg_upgrade-created target directory was removed');

done_testing();
