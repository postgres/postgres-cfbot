# Copyright (c) 2026, PostgreSQL Global Development Group

# The logical revert worker is the fourth path that makes a rollback's FILEOPS
# inverses durable, alongside online ROLLBACK, abort redo on a standby, and the
# startup undo phase (all three counted by 017).  0005 claims it batches the
# fsyncs the same way, but 017 never exercises the worker.  This test does.
#
# When the startup undo phase cannot complete a crash loser's FILEOPS chain
# (here, UndoReadBatchFromWAL is made to fail), the loser is deferred to the
# logical revert worker with ownership NOT taken (fileops_owned = false), and
# the worker applies the inverses itself.  N inverses on one file must then
# fsync the file and its directory exactly twice -- one batch per rollback,
# whatever the number of inverses -- counted through the "fileops-inverse-fsync"
# injection point, exactly as the other three paths.
#
# The counterpart is ownership: when the chain's FILEOPS inverses are already
# durable (an online abort whose abort WAL owns them, deferred to the ATM), the
# worker must NOT re-apply or re-fsync them.  That path drains with zero worker
# fsyncs.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# POSIX permission semantics; the inverse we count and re-check is chmod.
plan skip_all => 'POSIX permission semantics required' if $windows_os;

my $nops = 200;

# Which paths the "fileops-inverse-fsync" NOTICEs in $text name, how often.
sub synced
{
	my ($text) = @_;
	my %count;
	$count{$1}++
	  while $text =~
	  /notice triggered for injection point fileops-inverse-fsync \((.*?)\)/g;
	return \%count;
}

my $node = PostgreSQL::Test::Cluster->new('lrw');
$node->init;
# FILEOPS writes UNDO only when WAL is needed; keep the worker off until the
# loser's chain is in the ATM so we can count only the worker's fsyncs.  The
# preloaded module reports each inverse fsync and, for the first phase, makes
# the startup undo phase fail to read the chain.
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
autovacuum = off
fsync = on
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
shared_preload_libraries = 'test_fileops'
test_fileops.notice_inverse_fsync = on
test_fileops.fail_undo_batch_read = on
log_min_messages = notice
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION test_fileops');

my $path = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('lrw.dat')");
chmod(0644, $path) or die "chmod $path: $!";

my $chmods =
	"SELECT count(test_fileops_chmod('lrw.dat', 384 + (g % 2) * 36))"
  . " FROM generate_series(1, $nops) g";

# A transaction in flight when the server stops: a crash loser.  Its UNDO chain
# carries $nops chmod inverses on one file.
my $bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe($chmods);
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->stop('immediate');
$bg->{run}->finish;

# Restart.  The startup undo phase cannot read the chain (fail_undo_batch_read),
# so it applies none of the inverses and defers the loser to the logical revert
# worker with ownership NOT taken.  The worker is still held off.
$node->start;
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
	't',
	'startup deferred the un-applied FILEOPS loser to the ATM');
is((stat($path))[2] & 07777,
	0600, 'startup did not revert the mode (deferred)')
  or diag('mode is ' . sprintf('%04o', (stat($path))[2] & 07777));

# Now let the worker read the chain and apply the inverses itself.  Count the
# fsyncs it makes, from the server log where the preloaded point reports them.
$node->append_conf(
	'postgresql.conf', q{
test_fileops.fail_undo_batch_read = off
max_logical_revert_workers = 2
});
my $log_offset = -s $node->logfile;
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	'logical revert worker drained the ATM');

my $worklog = slurp_file($node->logfile, $log_offset);
my $worker = () = $worklog =~
  /notice triggered for injection point fileops-inverse-fsync \(/g;
is_deeply(
	synced($worklog),
	{ 'lrw.dat' => 1, '.' => 1 },
	'logical revert worker fsyncs the file and its directory, each once');
cmp_ok($worker, '>=', 1, 'logical revert worker makes its inverses durable');
cmp_ok($worker, '<=', 2,
	"logical revert worker of $nops inverses on one file fsyncs the file and its directory once each (got $worker)"
);
is((stat($path))[2] & 07777,
	0644, 'logical revert worker restored the original mode');

# Ownership: an online abort whose abort WAL already owns the FILEOPS inverses,
# deferred to the ATM, must NOT be re-applied or re-fsynced by the worker.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
$node->restart;
my $owned = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('owned.dat')");
chmod(0644, $owned) or die "chmod $owned: $!";
# Fail the inline rollback after its batch applies: the chmod is reverted
# inline (the abort WAL owns it) and the chain is handed to the ATM.
$bg = $node->background_psql('postgres');
$bg->query_safe('SELECT injection_points_set_local()');
$bg->query_safe(
	"SELECT injection_points_attach('undo-apply-after-batch', 'error')");
$bg->query_safe('BEGIN');
$bg->query_safe(
		"SELECT count(test_fileops_chmod('owned.dat', 384 + (g % 2) * 36))"
	  . " FROM generate_series(1, $nops) g");
$bg->query('ROLLBACK');
$bg->quit;
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
	't',
	'the owned online abort was handed to the ATM');
is((stat($owned))[2] & 07777,
	0644, 'the abort WAL reverted the owned mode inline');
# A later change the worker's second walk must not undo.
chmod(0640, $owned) or die "chmod $owned: $!";

$log_offset = -s $node->logfile;
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	'worker drained the owned ATM entry');
my $owned_log = slurp_file($node->logfile, $log_offset);
my $owned_fsyncs = () = $owned_log =~
  /notice triggered for injection point fileops-inverse-fsync \(/g;
is($owned_fsyncs, 0, 'worker does not fsync inverses the abort WAL owns');
is((stat($owned))[2] & 07777,
	0640, 'worker leaves the later change to the owned file intact');

$node->stop;
done_testing();
