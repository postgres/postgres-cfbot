
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A checkpoint must survive an undo segment file that the discard worker
# removes after the checkpoint wrote its pages.
#
# The checkpointer writes dirty buffers and then, separately, asks the kernel
# to write them back (checkpoint_flush_after) and fsyncs the files it was told
# about.  It keeps only buffer tags for both, and it keeps one undo segment
# open.  If the discard worker removes a segment in between, the checkpointer
# reopens it by name and finds nothing.  md.c ignores such a file for
# writeback ("just a hint") and reports ENOENT for an fsync, which sync.c
# retries after absorbing the cancel the remover sent.  The undo segment code
# raised an ERROR instead, failing the checkpoint; under a discard-heavy load
# (HammerDB TPROC-C) checkpoints failed this way.
#
# Each phase dirties a little more than one undo segment, holds the discard
# horizon back, starts a checkpoint and stops the checkpointer at writeback
# (phase 1) or at fsync (phase 2) of an undo segment, lets the discard worker
# remove the older segment, and lets the checkpointer go on.  The undo
# segment code now does as md.c does.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('undo_segment_removed');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
fsync = on
checkpoint_timeout = 1h
max_wal_size = 4GB
shared_buffers = 32MB
bgwriter_lru_maxpages = 0
checkpoint_flush_after = 2MB
shared_preload_libraries = 'injection_points'
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE t (id int PRIMARY KEY, v text) USING flux;
INSERT INTO t SELECT g, repeat('x', 1000) FROM generate_series(1, 100) g;
});

my $segsize = $node->safe_psql('postgres',
	q{SELECT current_setting('block_size')::int * 128});
my $s = $node->background_psql('postgres');
my $logno;

# The segment number holding the insert point of $s's undo log.
sub insert_segment
{
	return $s->query_safe(
		qq{SELECT (('x' || insert)::bit(64)::bigint & ((1::bigint << 40) - 1))
		          / $segsize
		     FROM pg_stat_get_undo_logs()
		    WHERE pid = pg_backend_pid() AND persistence = 'permanent'});
}

foreach my $point ('undofile-writeback', 'undofile-syncfiletag')
{
	$s->query_safe('UPDATE t SET v = v');
	$logno = $s->query_safe(
		q{SELECT logno FROM pg_stat_get_undo_logs()
		   WHERE pid = pg_backend_pid() AND persistence = 'permanent'});
	$s->query_safe('CHECKPOINT');

	# Hold the discard horizon back, then fill the rest of the insert
	# segment and start the next one: both are dirty now.
	my $hold = $node->background_psql('postgres');
	$hold->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;');
	my $old = insert_segment();
	my $i = 0;
	while (insert_segment() == $old)
	{
		$s->query_safe("UPDATE t SET v = repeat('$i', 1000)");
		$i++;
	}
	my $path = $node->data_dir
	  . sprintf('/base/undo/%06X.%010X', $logno, $old * $segsize);
	ok(-e $path, "$point: segment $old is there");

	$node->safe_psql('postgres',
		"SELECT injection_points_attach('$point', 'wait')");
	my $ck = $node->background_psql('postgres', on_error_stop => 0);
	my $log_offset = -s $node->logfile;
	$ck->query_until(qr/ck-start/, "\\echo ck-start\nCHECKPOINT;\n");
	$node->wait_for_event('checkpointer', $point);

	# The discard worker removes the older segment.
	$hold->query_safe('COMMIT');
	$hold->quit;
	ok( $node->poll_query_until(
			'postgres', qq{SELECT NOT EXISTS (
  SELECT FROM pg_ls_dir('base/undo') f
   WHERE f = '@{[ sprintf('%06X.%010X', $logno, $old * $segsize) ]}')}),
		"$point: the discard worker removes segment $old");

	$node->safe_psql(
		'postgres', qq{
SELECT injection_points_detach('$point');
SELECT injection_points_wakeup('$point');
});
	my ($out, $failed) = $ck->query('SELECT 1');
	$ck->quit;
	ok( !$node->log_contains('cannot open undo segment file', $log_offset),
		"$point: the checkpointer does not miss the removed segment");
	ok(!$failed, "$point: the CHECKPOINT succeeds");
}

$s->quit;
done_testing();
