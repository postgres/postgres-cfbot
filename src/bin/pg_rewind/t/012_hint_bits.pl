# Copyright (c) 2026, PostgreSQL Global Development Group

#
# Hint bits set on a hot standby are not WAL-logged.  If the standby replayed
# a commit or abort that the new primary never received, a hint bit for it
# must not survive pg_rewind.
#
# The same applies to hint bits set after a promotion, on pages that were
# last modified before the last replayed checkpoint, whether the buffer is
# still dirty from recovery or has been written out since.
#
# A clean restart of the primary must not keep a standby from setting hint
# bits for transactions that ended before it.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use File::Copy;
use Test::More;

# Run the scenario.  If $promote is true, the rewind target is promoted
# before the hint bit is set, otherwise it stays a standby.  If $evict is
# true, the buffer is written out before that.  If $restart is true, the
# last checkpoint before the fork is a shutdown checkpoint.
sub run_test
{
	my ($mode, $promote, $evict, $restart) = @_;

	my $node_a = PostgreSQL::Test::Cluster->new("a_$mode");
	$node_a->init(allows_streaming => 1, extra => ['-k']);
	$node_a->append_conf(
		'postgresql.conf', qq(
wal_log_hints = on
max_prepared_transactions = 5
shared_buffers = '128MB'
autovacuum = off
checkpoint_timeout = '1h'
max_wal_size = '1GB'
wal_keep_size = '100MB'
));
	$node_a->start;
	$node_a->safe_psql('postgres', 'CREATE EXTENSION pageinspect');
	if ($evict)
	{
		$node_a->safe_psql('postgres', 'CREATE EXTENSION pg_buffercache');
		$node_a->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	}

	$node_a->backup('b');
	my $node_s1 = PostgreSQL::Test::Cluster->new("s1_$mode");
	$node_s1->init_from_backup($node_a, 'b', has_streaming => 1);
	$node_s1->start;
	my $node_s2 = PostgreSQL::Test::Cluster->new("s2_$mode");
	$node_s2->init_from_backup($node_a, 'b', has_streaming => 1);
	$node_s2->start;

	$node_a->safe_psql('postgres',
		'CREATE TABLE c(id int); INSERT INTO c SELECT generate_series(1,100); CHECKPOINT;'
	);

	my $sess = $node_a->background_psql('postgres');
	$sess->query_safe('BEGIN; INSERT INTO c VALUES (999999);');
	my $xid = $sess->query_safe('SELECT pg_current_xact_id()');
	$sess->query_safe("PREPARE TRANSACTION 'p'");
	$sess->quit;

	# A second transaction changes the page right before the checkpoint
	# and aborts after the fork.  That leaves the page LSN exactly at the
	# redo pointer.  A restart would abort it, so skip it then.
	my $sess2;
	if ($restart)
	{
		$node_a->restart;
	}
	else
	{
		$sess2 = $node_a->background_psql('postgres');
		$sess2->query_safe('BEGIN; INSERT INTO c VALUES (888888);');
		$node_a->safe_psql('postgres', 'CHECKPOINT');
	}
	$node_a->wait_for_replay_catchup($node_s1);
	$node_a->wait_for_replay_catchup($node_s2);

	# Detach S1 from A and promote it.  This is the fork point.  Without
	# the checkpoint, replay on the rewound node would start at S1's old
	# restartpoint and the INSERT's FPW would hide the problem.
	$node_s1->append_conf('postgresql.conf', "primary_conninfo = ''");
	$node_s1->reload;
	$node_s1->poll_query_until('postgres',
		'SELECT count(*) = 0 FROM pg_stat_wal_receiver')
	  or die 'S1 did not disconnect';
	$node_s1->promote;
	$node_s1->safe_psql('postgres', 'CHECKPOINT');

	# S2 keeps following A and replays the rollback, which S1 never sees.
	$sess2->quit if defined $sess2;
	$node_a->safe_psql('postgres', "ROLLBACK PREPARED 'p'");

	# A checkpoint after the rollback must not let S2 set the hint bit
	$node_a->safe_psql('postgres', 'CHECKPOINT') if $restart;
	$node_a->wait_for_replay_catchup($node_s2);

	# 512 is HEAP_XMIN_INVALID
	my $hint_q =
	  "SELECT (t_infomask & 512) <> 0 FROM heap_page_items(get_raw_page('c', 0))
		 WHERE t_xmin::text = '$xid'";

	# Write out the buffer without a restartpoint, and hold back the
	# checkpoint that follows the promotion.  S2's redo pointer then stays
	# older than the page, so a hint bit set after the promotion would not
	# be WAL-logged on its own.
	if ($evict)
	{
		$node_s2->safe_psql(
			'postgres',
			"SELECT pg_buffercache_evict(bufferid) FROM pg_buffercache
			 WHERE relfilenode = pg_relation_filenode('c')");

		# Set the hint bits of both transactions while still in recovery.
		# The buffer is clean, so they stay in memory.  After the promotion
		# only harmless hint bits are left to set, and those must not take
		# these two to disk unlogged.
		$node_s2->safe_psql('postgres',
			"SELECT 1 FROM c WHERE ctid IN ('(0,101)', '(0,102)')");
		is($node_s2->safe_psql('postgres', $hint_q),
			't', "$mode: hint bit set in memory before promotion");
		$node_s2->safe_psql('postgres',
			"SELECT injection_points_attach('create-checkpoint-initial', 'wait')"
		);
	}

	if ($promote)
	{
		# Let S2 pick timeline 3, otherwise both nodes end up on timeline
		# 2 and pg_rewind sees nothing to do.
		copy(
			$node_s1->data_dir . '/pg_wal/00000002.history',
			$node_s2->data_dir . '/pg_wal/00000002.history'
		) or die "could not copy history file: $!";
		$node_s2->promote;
		$node_s2->wait_for_event('checkpointer', 'create-checkpoint-initial')
		  if $evict;
	}

	# Reading c tries to set the hint bit.  The page was last modified
	# before the last replayed checkpoint.
	is($node_s2->safe_psql('postgres', 'SELECT count(*) FROM c'),
		'100', "$mode: S2 does not see the rolled back row");
	if ($promote)
	{
		is($node_s2->safe_psql('postgres', $hint_q),
			't', "$mode: hint bit set on S2");
	}
	else
	{
		is($node_s2->safe_psql('postgres', $hint_q),
			'f', "$mode: hint bit not set on S2");

		# The other rows were committed before the last checkpoint started,
		# so their hint bits are fine.  256 is HEAP_XMIN_COMMITTED.
		is( $node_s2->safe_psql(
				'postgres',
				"SELECT bool_and((t_infomask & 256) <> 0)
				 FROM heap_page_items(get_raw_page('c', 0))
				 WHERE lp <= 100"),
			't',
			"$mode: hint bits of older transactions set on S2");
	}

	if ($evict)
	{
		$node_s2->safe_psql('postgres',
			"SELECT injection_points_wakeup('create-checkpoint-initial')");
		$node_s2->safe_psql('postgres',
			"SELECT injection_points_detach('create-checkpoint-initial')");
	}

	$node_s2->stop('fast');
	$node_a->stop('immediate');

	# S1 recovered the prepared transaction and commits it on its timeline.
	$node_s1->safe_psql('postgres', "COMMIT PREPARED 'p'");

	command_ok(
		[
			'pg_rewind',
			'--source-server' => $node_s1->connstr('postgres'),
			'--target-pgdata' => $node_s2->data_dir,
			'--write-recovery-conf',
		],
		"$mode: pg_rewind runs");

	# pg_rewind copied S1's postgresql.conf
	$node_s2->append_conf('postgresql.conf', 'port = ' . $node_s2->port);
	$node_s2->start;
	$node_s1->wait_for_replay_catchup($node_s2);

	is($node_s2->safe_psql('postgres', $hint_q),
		'f', "$mode: no hint bit on S2 after rewind");
	is( $node_s2->safe_psql(
			'postgres', 'SELECT count(*) FROM c WHERE id = 999999'),
		'1',
		"$mode: committed row visible on S2 after rewind");

	# Reading c on S1 emits an FPI_FOR_HINT that would mask the problem on
	# S2, so do this last.
	is( $node_s1->safe_psql(
			'postgres', 'SELECT count(*) FROM c WHERE id = 999999'),
		'1',
		"$mode: row visible on S1");

	$node_s1->stop;
	$node_s2->stop;
	return;
}

run_test('standby', 0, 0, 0);
run_test('standby_restart', 0, 0, 1);
run_test('promoted', 1, 0, 0);

# The last scenario needs an injection point to hold back the checkpoint.
run_test('promoted_clean', 1, 1, 0)
  if $ENV{enable_injection_points} eq 'yes';

done_testing();
