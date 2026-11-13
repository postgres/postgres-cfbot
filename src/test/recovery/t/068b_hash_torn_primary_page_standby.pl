# Copyright (c) 2026, PostgreSQL Global Development Group

# Test a standby that starts from a base backup whose copy of the primary
# bucket page of a hash bucket is torn, and the full-page image that repairs it
# comes after the record that reads it.
#

use strict;
use warnings FATAL => 'all';

use File::Path qw(rmtree);
use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

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

# One bucket receives every key: a primary page holding about 407 entries and
# an overflow page with the rest.
$primary->safe_psql(
	'postgres', q{
CREATE TABLE t (id int, k int) WITH (autovacuum_enabled = false);
CREATE INDEX t_i ON t USING hash (k);
INSERT INTO t SELECT i, 1 FROM generate_series(1, 600) i;
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

PostgreSQL::Test::RecursiveCopy::copypath($primary->data_dir, $backup_dir);
unlink("$backup_dir/postmaster.pid")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.pid");
unlink("$backup_dir/postmaster.opts")
  or BAIL_OUT("unable to unlink $backup_dir/postmaster.opts");
rmtree("$backup_dir/pg_wal")
  or BAIL_OUT("unable to unlink contents of $backup_dir/pg_wal");
mkdir("$backup_dir/pg_wal");

# While the backup is in progress: the entries of ids 451-600 are on the
# overflow page, those of ids 1-50 on the primary page.  The DELETE for the
# overflow page comes first; the VACUUM of the primary page then changes it for
# the first time since the checkpoint of the backup, so its record carries the
# full-page image of the page.
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id > 450');
$primary->safe_psql('postgres', 'VACUUM t');
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id <= 50');
$primary->safe_psql('postgres', 'VACUUM t');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Read the Hash records after the redo pointer of the backup.
my ($stdout, $stderr);
IPC::Run::run(
	[
		'pg_waldump',
		'--path' => $primary->data_dir . '/pg_wal',
		'--start' => $redo,
		'--rmgr' => 'Hash',
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
		&& $r =~ /blkref #[01]: rel \S+ blk (\d+)/)
	{
		$pblk = $1;
		last;
	}
}
ok(defined $pblk, 'premise: found the primary bucket page')
  or diag $stdout;

# The first DELETE for an overflow page.
my ($idx_delete) =
  grep { $records[$_] =~ /desc: DELETE .*is_primary F/ } 0 .. $#records;
ok(defined $idx_delete,
	'premise: VACUUM wrote a DELETE for an overflow page')
  or diag $stdout;

# Where the first full-page image of the primary page is, relative to the
# DELETE for the overflow page.
my ($idx_fpi) =
  defined $pblk
  ? grep { $records[$_] =~ /blkref #\d+: rel \S+ blk $pblk FPW/ }
  0 .. $#records
  : ();
ok(defined $idx_fpi,
	'premise: the primary page got a full-page image after the checkpoint')
  or diag $stdout;
if (defined $idx_delete && defined $idx_fpi)
{
	cmp_ok($idx_delete, '<', $idx_fpi,
		'premise: the DELETE comes before the image of the primary page');
}

# The copy of the primary page in the backup is torn: keep the first half of
# its block and overwrite the second half.
if (defined $pblk)
{
	my $path = "$backup_dir/$relpath";
	open(my $fh, '+<:raw', $path) or die "could not open $path: $!";
	sysseek($fh, $pblk * $bs + $bs / 2, 0) or die "seek: $!";
	my $junk = join('', map { chr(int(rand(256))) } 1 .. $bs / 2);
	syswrite($fh, $junk) == $bs / 2 or die "write: $!";
	close($fh);
}

# End the backup, and save backup_label into the backup.
my $backup_label = $psql->query_safe("select labelfile from pg_backup_stop()");
$psql->quit;
open(my $fh, '>', "$backup_dir/backup_label")
  or BAIL_OUT("unable to create $backup_dir/backup_label: $!");
print $fh $backup_label;
close($fh);

# Start a hot standby from the backup.  Replay starts at the redo pointer of
# the backup, replays the DELETE for the overflow page first, and reaches the
# consistency point only after the end of the backup, after the full-page
# image of the primary page.
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->append_conf('postgresql.conf', 'hot_standby = on');

my $started = $standby->start(fail_ok => 1);
ok($started, 'the standby starts');
if ($started)
{
	$primary->wait_for_catchup($standby);
	is( $standby->safe_psql(
			'postgres',
			q{SET enable_seqscan = off; SET enable_bitmapscan = off;
			  SELECT count(*) FROM t WHERE k = 1}),
		'400',
		'the hash index gives the right result on the standby');
}
else
{
	my $log = slurp_file($standby->logfile);
	diag("the standby did not start; the log has:\n"
		  . join('', grep { /invalid page|FATAL|WAL redo|consistent recovery/ } split(/^/m, $log)));
}

done_testing();
