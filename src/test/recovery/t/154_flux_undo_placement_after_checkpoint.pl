
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A FLUX undo record must replay where DO wrote it, even when a checkpoint
# begins while its forward WAL record is being built.
#
# DO reserves the record's undo space -- advancing the undo log's insert
# pointer -- before the critical section, decides whether the log's metadata
# needs a WAL record (LogUndoMetaData), and only then inserts the forward
# record that carries the undo.  A checkpoint that chooses its redo point
# after that decision but before the forward record, and snapshots the undo
# logs (pg_undo/<redo>) after the reservation, leaves crash recovery with a
# snapshot whose insert pointer is already past the record, and no metadata
# record after the redo point to put it back.  Redo then rebuilt the record one
# record further on and stopped with "FLUX undo redo: undo record pointer
# mismatch".  recovery/137 hit this about once in 70 runs under EXEC_BACKEND.
#
# The injection point flux-undo-before-forward-wal holds a FLUX writer in that
# window until the checkpoint has written its undo snapshot; the checkpoint
# then completes and the server crashes before the writer commits.  The same
# steps on a heap table are the control: heap writes no undo.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $n = 0;

# The undo snapshot files a checkpoint has written so far.
sub snapshots
{
	my ($node) = @_;
	my $dir = $node->data_dir . '/pg_undo';
	opendir(my $dh, $dir) or return ();
	my @f = grep { /^[0-9A-F]{16}$/ } readdir($dh);
	closedir($dh);
	return @f;
}

# In a session that first committed $pre (if any), and in a transaction that
# ran $first (if any), run $dml on a table of access method $am, with a
# checkpoint choosing its redo point and writing its undo snapshot while the
# writer sits between its undo insert and its forward WAL record.  Then crash
# before commit and restart.  Returns "count|sum" of the table after
# recovery, or undef if recovery failed.
sub crash_in_window
{
	my ($am, $pre, $first, $dml) = @_;
	my $node = PostgreSQL::Test::Cluster->new('flux_undo_placement_' . $n++);

	$node->init;
	$node->append_conf(
		'postgresql.conf', q{
autovacuum = off
shared_preload_libraries = 'injection_points'
});
	$node->start;
	$node->safe_psql(
		'postgres', qq{
CREATE EXTENSION injection_points;
CREATE TABLE t (id int, v int) USING $am WITH (autovacuum_enabled = off);
INSERT INTO t SELECT g, g FROM generate_series(1, 1000) g;
CHECKPOINT;
});

	my $h = $node->background_psql('postgres', on_error_stop => 0);
	$h->query_safe($pre) if $pre ne '';
	$h->query_safe('BEGIN');
	$h->query_safe($first) if $first ne '';

	# Park only this backend.  Only FLUX writers reach the point.
	$h->query_safe(
		q{SELECT injection_points_set_local(),
		         injection_points_attach('flux-undo-before-forward-wal', 'wait')}
	);
	$h->query_until(qr/start/, "\\echo start\n$dml;\n");
	$node->wait_for_event('client backend', 'flux-undo-before-forward-wal')
	  if $am eq 'flux';

	# The checkpoint writes its undo snapshot (CheckPointUndoLogs) before it
	# flushes buffers, which waits for the page the writer has locked.  So
	# wait for the snapshot, not for the checkpoint.
	my %before = map { $_ => 1 } snapshots($node);
	my $ckpt = $node->background_psql('postgres', on_error_stop => 0);
	# A checkpoint that nothing holds up (the heap control) can finish before
	# the first output is read, and query_until() discards what it read.
	my $ckpt_out = $ckpt->query_until(qr/ckpt/,
		"\\echo ckpt\nCHECKPOINT;\n\\echo ckpt-done\n");
	my $deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
	until (grep { !$before{$_} } snapshots($node))
	{
		die "no new undo snapshot" if time() > $deadline;
		select(undef, undef, undef, 0.05);
	}

	$node->safe_psql('postgres',
		q{SELECT injection_points_wakeup('flux-undo-before-forward-wal')})
	  if $am eq 'flux';

	# The forward record is now after the new redo point.  Let the
	# checkpoint finish, so that recovery starts from it, then crash before
	# the writer commits.
	$h->query_until(qr/done/, "\\echo done\n");
	$ckpt->query_until(qr/ckpt-done/, '') unless $ckpt_out =~ /ckpt-done/;
	$ckpt->quit;
	$node->stop('immediate');
	eval { $h->quit };

	if (!$node->start(fail_ok => 1))
	{
		diag($_)
		  for grep { /PANIC|FATAL/ } split /\n/, slurp_file($node->logfile);
		return undef;
	}
	my $res = $node->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t');
	$node->stop;
	return $res;
}

for my $am ('heap', 'flux')
{
	# The first undo record of a transaction, in a new undo log.
	is( crash_in_window($am, '', '', 'DELETE FROM t WHERE id = 1'),
		'1000|500500',
		"$am: first undo record of a transaction, rolled back after crash");

	# The first undo record of a transaction that follows a committed one in
	# the same undo log, whose transaction header gets linked to it.
	is( crash_in_window(
			$am, 'UPDATE t SET v = v + 1 WHERE id = 2',
			'', 'DELETE FROM t WHERE id = 1'),
		'1000|500501',
		"$am: first undo record after a committed transaction, rolled back after crash"
	);

	# A later record of a transaction that wrote undo before the checkpoint.
	is( crash_in_window(
			$am, '',
			'UPDATE t SET v = v + 1 WHERE id <= 10',
			'UPDATE t SET v = v + 1 WHERE id = 500'),
		'1000|500500',
		"$am: later undo record of a transaction, rolled back after crash");

	# The first undo record of a transaction, written in a subtransaction.
	is( crash_in_window(
			$am, '', 'SAVEPOINT s', 'UPDATE t SET v = v + 1 WHERE id = 500'),
		'1000|500500',
		"$am: first undo record in a subtransaction, rolled back after crash"
	);
}

done_testing();
