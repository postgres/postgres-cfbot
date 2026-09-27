# Copyright (c) 2026, PostgreSQL Global Development Group

# Abort applies FILEOPS inverses after the abort record is flushed and clog
# says aborted.  A failed fsync of an inverse must PANIC there even with
# data_sync_retry = on: an ERROR would re-enter AbortTransaction and log a
# second abort record.  Crash recovery replays the abort record instead.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('inverse_fsync');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_logical_revert_workers = 0
autovacuum = off
fsync = on
data_sync_retry = on
restart_after_crash = on
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
my $dir = $node->data_dir;
my $start_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

$node->safe_psql('postgres',
	"SELECT injection_points_attach('fileops-inverse-fsync-fail', 'notice')");
my $log_offset = -s $node->logfile;
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', q{
BEGIN;
SELECT test_fileops_create('inverse.dat', 384);
ROLLBACK;
});

# The server restarts after the PANIC, which also drops the attachment.
# Wait for the restarted server to report that it is ready.  Polling with a
# query is not enough: a connection made before the postmaster notices the
# PANIC reaches the old server and succeeds, and the next query then finds
# the server in crash recovery.
$node->wait_for_log(qr/database system is ready to accept connections/,
	$log_offset);
my $log = slurp_file($node->logfile);
like(
	$log,
	qr/PANIC:  could not fsync file ".*inverse\.dat"/,
	'failed inverse fsync during abort is a PANIC');
unlike(
	$log,
	qr/AbortTransaction while in ABORT state/,
	'abort is not re-entered');

my $end_lsn =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
my ($walout, $walerr);
IPC::Run::run [
	'pg_waldump',
	'--path' => "$dir/pg_wal",
	'--start' => $start_lsn,
	'--end' => $end_lsn,
	'--rmgr' => 'Transaction'
  ],
  '>' => \$walout,
  '2>' => \$walerr;
my $aborts = () = $walout =~ /desc: ABORT/g;
is($aborts, 1, 'exactly one abort record');
ok(!-e "$dir/inverse.dat", 'crash recovery removed the aborted create');

$node->safe_psql('postgres',
	"BEGIN; SELECT test_fileops_create('after.dat', 384); ROLLBACK;");
ok(!-e "$dir/after.dat", 'later aborts still apply their inverses');

$node->stop;
done_testing();
