# Copyright (c) 2026, PostgreSQL Global Development Group

# Test redo of a hash VACUUM record that reads a torn primary bucket page, whose
# repairing full-page image comes later in the WAL, in a standby started from a
# base backup and in crash recovery.  Data checksums must be on, so that the
# torn page is detected.

use strict;
use warnings FATAL => 'all';

use File::Path qw(rmtree);
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Tear a block of a relation file, as a torn write does: keep the first half of
# the block and overwrite the second half with random bytes, so that its
# checksum does not match.
sub tear_page
{
	my ($path, $pblk, $bs) = @_;

	open(my $fh, '+<:raw', $path) or die "could not open $path: $!";
	sysseek($fh, $pblk * $bs + $bs / 2, 0) or die "seek: $!";
	my $junk = join('', map { chr(int(rand(256))) } 1 .. $bs / 2);
	syswrite($fh, $junk) == $bs / 2 or die "write: $!";
	close($fh);
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
autovacuum = off
fsync = off
full_page_writes = on
checkpoint_timeout = 1h
});
$primary->start;

is($primary->safe_psql('postgres', 'SHOW data_checksums'),
	'on', 'premise: data checksums are on');

# One bucket receives every key: a primary bucket page and one overflow page.
# fillfactor keeps the heap row count per page fixed across architectures, and
# vacuuming the empty table first makes hashbuild() start the index with the
# minimum two buckets, so the layout does not depend on pointer width.
$primary->safe_psql('postgres', 'CREATE EXTENSION pageinspect');
$primary->safe_psql(
	'postgres', q{
CREATE TABLE t (k int) WITH (autovacuum_enabled = false, fillfactor = 100);
VACUUM t;
CREATE INDEX t_i ON t USING hash (k);
INSERT INTO t SELECT 1 FROM generate_series(1, 600);
});
my $relpath = $primary->safe_psql('postgres',
	q{SELECT pg_relation_filepath('t_i')});
my $bs = $primary->safe_psql('postgres', 'SHOW block_size');

# Start a low-level backup, which does a checkpoint, and copy the files.
my $backup_name = 'torn';
my $backup_dir = $primary->backup_dir . '/' . $backup_name;
my $psql = $primary->background_psql('postgres');
$psql->query_safe("SET client_min_messages TO WARNING");
$psql->set_query_timer_restart;
$psql->query_safe("select pg_backup_start('torn primary page', true)");
my $redo = $primary->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');

# Copy the data directory of the running server, as a backup does.  Remove what
# must not be in a backup, as pg_basebackup does: postmaster.pid, which would
# make a server started from the copy think that the primary's postmaster is
# still running, postmaster.opts, which only records how the primary was
# started, and the WAL segments, which are copied at an arbitrary moment, and
# which the standby gets by streaming, starting at the redo pointer of the
# backup.  The server expects pg_wal to exist, so create it empty.
PostgreSQL::Test::RecursiveCopy::copypath($primary->data_dir, $backup_dir);
unlink("$backup_dir/postmaster.pid")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.pid");
unlink("$backup_dir/postmaster.opts")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.opts");
rmtree("$backup_dir/pg_wal")
  or BAIL_OUT("unable to unlink contents of $backup_dir/pg_wal");
mkdir("$backup_dir/pg_wal");

# While the backup is in progress: delete exactly the index entries that live
# on the overflow pages, so VACUUM removes dead entries only from the overflow
# chain and never from the primary bucket page.  VACUUM then logs an
# XLOG_HASH_DELETE for the overflow page while holding the primary bucket page
# pin-only (not registered), and the squeeze that follows is the first record
# to change the primary page since the backup checkpoint, so it carries the
# full-page image that repairs the torn page on replay.  Keying the delete off
# the actual page contents keeps this true regardless of how many entries fit
# on a page, which varies by architecture.
$primary->safe_psql(
	'postgres', q{
DELETE FROM t WHERE ctid IN (
  SELECT i.ctid
  FROM generate_series(1, pg_relation_size('t_i') / current_setting('block_size')::int - 1) b
  CROSS JOIN LATERAL hash_page_items(get_raw_page('t_i', b)) i
  WHERE hash_page_type(get_raw_page('t_i', b)) = 'overflow');
VACUUM t;
});

# The number of live entries left, which the index must return after recovery.
# It depends on how many entries fit on a page, so read it rather than assume.
my $live = $primary->safe_psql('postgres', 'SELECT count(*) FROM t WHERE k = 1');

# Read the Hash records written since the checkpoint.
my $end_lsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# The first and the last WAL segment of the range.
my $start_seg =
  $primary->safe_psql('postgres', "SELECT pg_walfile_name('$redo')");
my $end_seg =
  $primary->safe_psql('postgres', "SELECT pg_walfile_name('$end_lsn')");

# Dump the Hash records from the redo pointer of the backup to the end of the
# WAL written so far. The output is parsed below.
my ($stdout, $stderr);
IPC::Run::run(
	[
		'pg_waldump',
		'--path' => $primary->data_dir . '/pg_wal',
		'--start' => $redo,
		'--end' => $end_lsn,
		'--rmgr' => 'Hash',
		$start_seg, $end_seg,
	],
	'>' => \$stdout,
	'2>' => \$stderr);
my @records = grep { /^rmgr: Hash/ } split(/^(?=rmgr: )/m, $stdout);

# The primary page: a DELETE or SQUEEZE_PAGE marked is_primary T has it as
# block 1, the page that is changed.
my $pblk;
for my $r (@records)
{
	if ($r =~ /desc: (?:DELETE|SQUEEZE_PAGE) .*is_primary T/
		&& $r =~ /blkref #1: rel \S+ blk (\d+)/)
	{
		$pblk = $1;
		last;
	}
}
ok(defined $pblk, 'premise: found the primary bucket page')
  or diag $stdout, $stderr;

# The DELETE for an overflow page has to come before the first full-page image
# of the primary page.
my ($idx_delete) =
  grep { $records[$_] =~ /desc: DELETE .*is_primary F/ } 0 .. $#records;
ok(defined $idx_delete, 'premise: VACUUM wrote a DELETE for an overflow page')
  or diag $stdout, $stderr;

my ($idx_fpi) =
  defined $pblk
  ? grep { $records[$_] =~ /blkref #\d+: rel \S+ blk $pblk FPW/ }
  0 .. $#records
  : ();
ok(defined $idx_fpi,
	'premise: the primary page got a full-page image after the checkpoint')
  or diag $stdout, $stderr;
if (defined $idx_delete && defined $idx_fpi)
{
	cmp_ok($idx_delete, '<', $idx_fpi,
		'premise: the DELETE comes before the image of the primary page');
}

# Tear the copy of the primary page in the backup.
tear_page("$backup_dir/$relpath", $pblk, $bs) if defined $pblk;

# End the backup, and save backup_label into the backup.
my $backup_label = $psql->query_safe("select labelfile from pg_backup_stop()");
$psql->quit;
open(my $fh, '>', "$backup_dir/backup_label")
  or BAIL_OUT("unable to create $backup_dir/backup_label: $!");
print $fh $backup_label;
close($fh);

###############################################################################
# A standby started from the backup
###############################################################################

# Replay starts at the redo pointer of the backup, replays the DELETE for the
# overflow page first, and reaches the consistency point only after the end of
# the backup, after the full-page image of the primary page.
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->append_conf('postgresql.conf', 'hot_standby = on');

my $started = $standby->start(fail_ok => 1);
ok($started, 'standby: the standby starts');
if ($started)
{
	$primary->wait_for_catchup($standby);
	is( $standby->safe_psql(
			'postgres',
			q{SET enable_seqscan = off; SET enable_bitmapscan = off;
			  SELECT count(*) FROM t WHERE k = 1}),
		$live,
		'standby: the hash index gives the right result');
	$standby->stop;
}
else
{
	my $log = slurp_file($standby->logfile);
	diag("standby: the standby did not start; the log has:\n"
		  . join('', grep { /invalid page|FATAL|WAL redo|consistent recovery/ } split(/^/m, $log)));
}

###############################################################################
# Crash recovery of the primary
###############################################################################

# There was no checkpoint since the one of the backup, so crash recovery
# replays the same records from the same redo pointer.  Tear the primary's own
# copy of the page.
$primary->stop('immediate');
tear_page($primary->data_dir . '/' . $relpath, $pblk, $bs) if defined $pblk;

$started = $primary->start(fail_ok => 1);
ok($started, 'crash recovery: the server starts');
if ($started)
{
	is( $primary->safe_psql(
			'postgres',
			q{SET enable_seqscan = off; SET enable_bitmapscan = off;
			  SELECT count(*) FROM t WHERE k = 1}),
		$live,
		'crash recovery: the hash index gives the right result');
}
else
{
	my $log = slurp_file($primary->logfile);
	diag("crash recovery: the server did not start; the log has:\n"
		  . join('', grep { /invalid page|FATAL|WAL redo/ } split(/^/m, $log)));
}

done_testing();
