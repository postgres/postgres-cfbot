# Copyright (c) 2026, PostgreSQL Global Development Group

# Test crash recovery when the primary bucket page of a hash bucket is torn,
# and the full-page image that repairs it comes after the record that reads it.
#

use strict;
use warnings FATAL => 'all';

use IPC::Run;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
fsync = off
full_page_writes = on
checkpoint_timeout = 1h
});
$node->start;

is($node->safe_psql('postgres', 'SHOW data_checksums'),
	'on', 'premise: data checksums are on');

# One bucket receives every key: a primary page holding about 407 entries and
# an overflow page with the rest.
$node->safe_psql(
	'postgres', q{
CREATE TABLE t (id int, k int) WITH (autovacuum_enabled = false);
CREATE INDEX t_i ON t USING hash (k);
INSERT INTO t SELECT i, 1 FROM generate_series(1, 600) i;
});
$node->safe_psql('postgres', 'CHECKPOINT');
my $redo = $node->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');

# The entries of ids 451-600 are on the overflow page, those of ids 1-50 on the
# primary page.  The DELETE for the overflow page comes first; the VACUUM of
# the primary page then changes it for the first time since the checkpoint, so
# its record carries the full-page image of the page.
$node->safe_psql('postgres', 'DELETE FROM t WHERE id > 450');
$node->safe_psql('postgres', 'VACUUM t');
$node->safe_psql('postgres', 'DELETE FROM t WHERE id <= 50');
$node->safe_psql('postgres', 'VACUUM t');

my $relpath = $node->safe_psql('postgres',
	q{SELECT pg_relation_filepath('t_i')});
my $bs = $node->safe_psql('postgres', 'SHOW block_size');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

# Read the Hash records after the checkpoint.
my ($stdout, $stderr);
IPC::Run::run(
	[
		'pg_waldump',
		'--path' => $node->data_dir . '/pg_wal',
		'--start' => $redo,
		'--rmgr' => 'Hash',
	],
	'>' => \$stdout,
	'2>' => \$stderr);
my @records = grep { /^rmgr: Hash/ } split(/^(?=rmgr: )/m, $stdout);

# The primary bucket page's block number is not a registered block of the
# overflow-page DELETE (the record carries it only as appended main data), so
# find it from a record that does register it: a DELETE or SQUEEZE_PAGE marked
# is_primary T references the primary bucket page as its first block.
my $pblk;
for my $r (@records)
{
	if ($r =~ /desc: (?:DELETE|SQUEEZE_PAGE) .*is_primary T/
		&& $r =~ /blkref #0: rel \S+ blk (\d+)/)
	{
		$pblk = $1;
		last;
	}
}
ok(defined $pblk, 'premise: found the primary bucket page')
  or diag $stdout;

# The overflow-page DELETE (is_primary F) reads the primary bucket page
# pin-only to take the cleanup lock; it must come before the primary page's
# first full-page image.
my ($idx_delete) =
  grep { $records[$_] =~ /desc: DELETE .*is_primary F/ } 0 .. $#records;
ok(defined $idx_delete,
	'premise: VACUUM wrote a DELETE for an overflow page')
  or diag $stdout;

# Where the first full-page image of the primary page is.
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

$node->stop('immediate');

# Simulate a torn write of the primary page: keep the first half of its block
# and overwrite the second half.
if (defined $pblk)
{
	my $path = $node->data_dir . '/' . $relpath;
	open(my $fh, '+<:raw', $path) or die "could not open $path: $!";
	sysseek($fh, $pblk * $bs + $bs / 2, 0) or die "seek: $!";
	my $junk = join('', map { chr(int(rand(256))) } 1 .. $bs / 2);
	syswrite($fh, $junk) == $bs / 2 or die "write: $!";
	close($fh);
}

my $started = $node->start(fail_ok => 1);
ok($started, 'crash recovery succeeds');
if ($started)
{
	is( $node->safe_psql(
			'postgres',
			q{SET enable_seqscan = off; SET enable_bitmapscan = off;
			  SELECT count(*) FROM t WHERE k = 1}),
		'400',
		'the hash index gives the right result after recovery');
	$node->stop;
}
else
{
	my $log = slurp_file($node->logfile);
	diag("the server did not start; the log has:\n"
		  . join('', grep { /invalid page|FATAL|WAL redo/ } split(/^/m, $log)));
}

done_testing();
