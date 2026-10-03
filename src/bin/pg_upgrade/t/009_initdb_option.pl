# Copyright (c) 2026, PostgreSQL Global Development Group

# Test the --initdb option of pg_upgrade: pg_upgrade creates the new cluster
# itself via initdb, instead of requiring the user to have run initdb first.

use strict;
use warnings FATAL => 'all';

use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize and populate the old cluster.
#
# Use non-default settings that --initdb must carry over to the new cluster
# (derived from the old cluster's pg_control): disabled data checksums (initdb
# enables them by default since PG18), a non-default WAL segment size, and the
# C locale.  We check below that the new cluster inherits them.
my $oldnode = PostgreSQL::Test::Cluster->new('old_node');
$oldnode->init(
	extra => [
		'--no-data-checksums',
		'--wal-segsize' => '2',
		'--locale' => 'C',
	]);
$oldnode->start;
$oldnode->safe_psql('postgres',
	    "CREATE TABLE t (id int primary key, note text); "
	  . "INSERT INTO t SELECT g, 'row ' || g FROM generate_series(1, 100) g; "
	  . "CREATE DATABASE extra_db;");
my $rows_before =
  $oldnode->safe_psql('postgres', 'SELECT count(*) FROM t');
is($rows_before, '100', 'old cluster has expected rows before upgrade');

# Record the old cluster's settings so we can compare them after the upgrade.
my $old_checksums = $oldnode->safe_psql('postgres', 'SHOW data_checksums');
my $old_wal_segsize = $oldnode->safe_psql('postgres', 'SHOW wal_segment_size');
my $old_encoding = $oldnode->safe_psql('postgres',
	"SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = 'template0'");
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

# Run pg_upgrade with --initdb.  We must run in a writable directory because
# pg_upgrade writes output files relative to the current directory.
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
	],
	'run of pg_upgrade --initdb creates and upgrades the new cluster');

# The new data directory should now exist and be a v18+ cluster.
ok(-f $newnode->data_dir . '/PG_VERSION',
	'new cluster data directory created by --initdb');

# The framework's init() would normally write port/socket settings into
# postgresql.conf; since we skipped it, append them now so we can start the
# upgraded cluster through the test harness.  Mirror init()'s own TCP vs Unix
# socket handling so this works on Windows (where TCP is used) as well.
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

# Verify the new cluster is a newer major version than the old one.
my $newver = $newnode->safe_psql('postgres',
	"SELECT current_setting('server_version_num')::int / 10000");
ok($newver >= 18, "new cluster reports target major version ($newver)");

# --initdb must reproduce these settings from the old cluster; otherwise
# check_control_data() would reject the new cluster.  Verify each carried over.
my $new_checksums = $newnode->safe_psql('postgres', 'SHOW data_checksums');
is($new_checksums, $old_checksums,
	"data_checksums propagated by --initdb ($new_checksums)");

my $new_wal_segsize = $newnode->safe_psql('postgres', 'SHOW wal_segment_size');
is($new_wal_segsize, $old_wal_segsize,
	"wal_segment_size propagated by --initdb ($new_wal_segsize)");

my $new_encoding = $newnode->safe_psql('postgres',
	"SELECT pg_encoding_to_char(encoding) FROM pg_database WHERE datname = 'template0'");
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

# --initdb must refuse to clobber an already-populated data directory, and the
# failure must come from pg_upgrade's own PG_VERSION check (not initdb's
# "directory not empty" error), so confirm the specific message.  pg_upgrade
# prints its fatal message to stdout, so match there.
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

# --check --initdb performs validation without creating anything.
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
	[qr/created with settings matching the old cluster/],
	[qr/^$/],
	'--check --initdb validates without creating the cluster');

# Verify that --check --initdb didn't create anything.
ok(!-d $newnode->data_dir . '_dry_run',
	'--check --initdb does not create the new cluster directory');

# -O passes postmaster-only options, which initdb does not accept, so the
# combination is rejected during option parsing rather than forwarded.
command_checks_all(
	[
		'pg_upgrade', '--no-sync',
		'--old-datadir' => $oldnode->data_dir,
		'--new-datadir' => $newnode->data_dir . '_nonexistent',
		'--old-bindir' => $oldbindir,
		'--new-bindir' => $newbindir,
		'--socketdir' => $newnode->host,
		'--old-port' => $oldnode->port,
		'--new-port' => $newnode->port,
		'--initdb',
		'--new-options' => '-c work_mem=1MB',
	],
	1,
	[qr/options -O\/--new-options and --initdb cannot be used together/],
	[qr/^$/],
	'--initdb and -O cannot be used together');

done_testing();
