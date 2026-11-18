# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# An ordinary filesystem error in an immediate FileOps operation (permission
# denied, no space, ...) must be a plain ERROR, exactly as without FILEOPS:
# the server must not PANIC, and the failed operation must leave nothing in
# the WAL for a standby to replay.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($windows_os)
{
	plan skip_all => 'POSIX permission semantics required';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_fs_errors');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = replica\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $datadir = $node->data_dir;
my $ro = "$datadir/ro_dir";
mkdir($ro) or die "mkdir $ro: $!";
my $file = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('fserr.dat')");
my $rofile = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('fserr_ro.dat')");
my $has_xattr = $node->safe_psql('postgres',
	"SELECT test_fileops_setxattr('$rofile', 'user.pgtest', 'v')");
chmod(0500, $ro) or die "chmod $ro: $!";
chmod(0444, $rofile) or die "chmod $rofile: $!";

my $start_time =
  $node->safe_psql('postgres', 'SELECT pg_postmaster_start_time()');
my $start_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

my %cases = (
	create => "SELECT test_fileops_create('$ro/new.dat', 384)",
	write => "SELECT test_fileops_write('/dev/full', 0, 'x')",
	truncate => "SELECT test_fileops_truncate('$rofile', 10)",
	chmod => "SELECT test_fileops_chmod('/', 448)",
	chown => "SELECT test_fileops_chown('$file', 0, 0)",
	mkdir => "SELECT test_fileops_mkdir('$ro/sub', 448)",
	symlink => "SELECT test_fileops_symlink('$file', '$ro/lnk')",
	link => "SELECT test_fileops_link('$file', '$ro/hard')",);

if ($has_xattr eq 't')
{
	$cases{setxattr} =
	  "SELECT test_fileops_setxattr('$rofile', 'user.pgtest2', 'v')";
	$cases{removexattr} =
	  "SELECT test_fileops_removexattr('$rofile', 'user.pgtest')";
}

foreach my $op (sort keys %cases)
{
	my ($ret, $stdout, $stderr) = $node->psql('postgres', $cases{$op});

	# The xattr wrappers report a refused operation as false.
	ok( ($stderr =~ /ERROR:/ && $stderr !~ /PANIC|FATAL/)
		  || ($stdout eq 'f' && $stderr eq ''),
		"$op: a filesystem error is an ERROR, not a PANIC"
	) or diag("stdout: $stdout\nstderr: $stderr");
	is($node->safe_psql('postgres', 'SELECT pg_postmaster_start_time()'),
		$start_time, "$op: the server did not restart");
}

unlike(
	slurp_file($node->logfile),
	qr/PANIC|terminated by signal/,
	'no PANIC or backend crash in the server log');

# CREATE TABLESPACE on a directory we do not own fails as it does without
# FILEOPS.  "/" is owned by root, and the server never runs as root.
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', "CREATE TABLESPACE t LOCATION '/'");
like(
	$stderr,
	qr/ERROR:  could not set permissions on directory "\/": (?:Operation not permitted|Read-only file system)/,
	'CREATE TABLESPACE on a root-owned directory gives the usual error');
is($node->safe_psql('postgres', 'SELECT pg_postmaster_start_time()'),
	$start_time, 'the server survived CREATE TABLESPACE');

# Nothing the failed operations attempted reached the WAL.
my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my ($walout, $walerr);
IPC::Run::run [
	'pg_waldump',
	'--path' => "$datadir/pg_wal",
	'--start' => $start_lsn,
	'--end' => $end_lsn,
	'--rmgr' => 'FileOps'
  ],
  '>' => \$walout,
  '2>' => \$walerr;
is($walout, '', 'no FILEOPS WAL record was written for a failed operation');

chmod(0700, $ro);
chmod(0644, $rofile);
$node->stop;
done_testing();
