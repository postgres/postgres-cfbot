# Copyright (c) 2026, PostgreSQL Global Development Group

# Rolling back N FILEOPS operations on one file must not fsync that file and
# its directory N times.  An inverse must be durable before the rollback is
# considered done, but one fsync of each distinct path (and its parent) per
# rollback is enough: the inverses of one rollback are made durable together,
# before anything records the rollback as complete.  Count the fsyncs in each
# apply path through the "fileops-inverse-fsync" injection point, which fires
# once per fsync and names the path: online ROLLBACK, abort redo on a
# standby, and the startup undo phase for a crash loser.  Each must fsync the
# file and its directory: two fsyncs, whatever the number of inverses.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $nops = 200;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', q{
wal_level = replica
autovacuum = off
fsync = on
});
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
$primary->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$primary->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('many.dat')");
my $path = $primary->data_dir . '/many.dat';

$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', 'log_min_messages = notice');
$standby->start;

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

my $chmods =
	"SELECT count(test_fileops_chmod('many.dat', 384 + (g % 2) * 36))"
  . " FROM generate_series(1, $nops) g";

# Online rollback: count the NOTICEs in the rolling-back session.
my ($ret, $stdout, $stderr) = $primary->psql(
	'postgres', qq{
SELECT injection_points_set_local();
SELECT injection_points_attach('fileops-inverse-fsync', 'notice');
BEGIN;
$chmods;
ROLLBACK;
});
is($ret, 0, 'online rollback succeeded');
my $online = () =
  $stderr =~ /notice triggered for injection point fileops-inverse-fsync \(/g;
is_deeply(
	synced($stderr),
	{ 'many.dat' => 1, '.' => 1 },
	'online rollback fsyncs the file and its directory, each once');
cmp_ok($online, '>=', 1, 'online rollback makes its inverses durable');
cmp_ok($online, '<=', 2,
	"online rollback of $nops inverses on one file fsyncs the file and its directory once each (got $online)"
);
is((stat($path))[2] & 07777, 0600, 'online rollback restored the mode')
  unless $windows_os;

# Abort redo on the standby: the standby applies the inverses carried in the
# abort record.  The point is attached in the standby's shared memory, where
# the startup process sees it, and reports to the standby's log.
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres',
	"SELECT injection_points_attach('fileops-inverse-fsync', 'notice')");
my $log_offset = -s $standby->logfile;
$primary->safe_psql('postgres', "BEGIN; $chmods; ROLLBACK;");
$primary->wait_for_replay_catchup($standby);
my $redo_log = slurp_file($standby->logfile, $log_offset);
my $redo = () = $redo_log =~
  /notice triggered for injection point fileops-inverse-fsync \(/g;
is_deeply(
	synced($redo_log),
	{ 'many.dat' => 1, '.' => 1 },
	'abort redo fsyncs the file and its directory, each once');
cmp_ok($redo, '>=', 1, 'abort redo makes its inverses durable');
cmp_ok($redo, '<=', 2,
	"abort redo of $nops inverses on one file fsyncs the file and its directory once each (got $redo)"
);
$standby->safe_psql('postgres',
	"SELECT injection_points_detach('fileops-inverse-fsync')");

# Crash loser: the transaction is in flight when the primary stops, so the
# startup undo phase applies its inverses from the UNDO chain.  Injection
# points do not survive the restart; attach this one from postgresql.conf
# through test_fileops, which the startup process loads.  Checkpoint first,
# so that crash recovery does not also replay the abort records above.
$primary->safe_psql('postgres', 'CHECKPOINT');
my $bg = $primary->background_psql('postgres');
$bg->query_safe('BEGIN');
$bg->query_safe($chmods);
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->stop('immediate');
$bg->{run}->finish;
$primary->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_fileops'
test_fileops.notice_inverse_fsync = on
log_min_messages = notice
});
$log_offset = -s $primary->logfile;
$primary->start;
my $startup_log = slurp_file($primary->logfile, $log_offset);
my $startup = () = $startup_log =~
  /notice triggered for injection point fileops-inverse-fsync \(/g;
is_deeply(
	synced($startup_log),
	{ 'many.dat' => 1, '.' => 1 },
	'startup undo fsyncs the file and its directory, each once');
cmp_ok($startup, '>=', 1, 'startup undo makes its inverses durable');
cmp_ok($startup, '<=', 2,
	"startup undo of $nops inverses on one file fsyncs the file and its directory once each (got $startup)"
);
is((stat($path))[2] & 07777, 0600, 'startup undo restored the mode')
  unless $windows_os;

$standby->stop;
$primary->stop;
done_testing();
