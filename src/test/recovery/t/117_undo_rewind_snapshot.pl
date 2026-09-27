
# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_rewind copies pg_undo/ whole from the source, whose checkpoints have
# already pruned the snapshot for the target's restart checkpoint (the last
# common one).  The rewound server must still replay the per-backend undo
# log records written after that checkpoint and come up with the source's
# data.  The heap twin is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $am (qw(heap flux))
{
	my $primary = PostgreSQL::Test::Cluster->new("primary_$am");
	$primary->init(allows_streaming => 1);
	$primary->append_conf(
		'postgresql.conf', qq[
wal_log_hints = on
wal_keep_size = 320MB
undo_worker_naptime = 10
]);
	$primary->start;
	$primary->safe_psql(
		'postgres', qq[
		CREATE EXTENSION amcheck;
		CREATE TABLE t (id int PRIMARY KEY, v int) USING $am;
		INSERT INTO t SELECT g, 0 FROM generate_series(1, 2000) g;
		UPDATE t SET v = v + 1;
		CHECKPOINT;
	]);
	$primary->backup('b');
	my $standby = PostgreSQL::Test::Cluster->new("standby_$am");
	$standby->init_from_backup($primary, 'b', has_streaming => 1);
	$standby->start;

	# Undo activity and the last common checkpoint.
	$primary->safe_psql('postgres', 'UPDATE t SET v = v + 1; CHECKPOINT;');
	$primary->wait_for_catchup($standby);
	$standby->promote;

	# Undo writes, discards and checkpoints on the new primary only: the
	# checkpoints prune the common checkpoint's pg_undo snapshot.
	for my $i (1 .. 3)
	{
		$standby->safe_psql('postgres', 'UPDATE t SET v = v + 1');
		$standby->poll_query_until('postgres',
			"SELECT coalesce(bool_and(discard = insert), true) FROM pg_stat_get_undo_logs() WHERE persistence = 'permanent'"
		) or die "timed out waiting for undo discard";
		$standby->safe_psql('postgres', 'CHECKPOINT');
	}
	my $expected =
	  $standby->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t');

	# Diverge the old primary.
	$primary->safe_psql('postgres', 'UPDATE t SET v = v + 100');
	$primary->stop;
	$standby->stop;

	command_ok(
		[
			'pg_rewind',
			'--target-pgdata' => $primary->data_dir,
			'--source-pgdata' => $standby->data_dir,
		],
		"$am: pg_rewind");

	$primary->append_conf('postgresql.conf',
		'port = ' . $primary->port . "\n");
	$primary->enable_streaming($standby);
	$standby->start;
	my $logstart = -s $primary->logfile;
	my $started = $primary->start(fail_ok => 1);
	ok($started, "$am: rewound server starts");
	if (!$started)
	{
		diag(slurp_file($primary->logfile, $logstart));
		$standby->stop;
		next;
	}

	# More undo after the rewind must replay too.
	$standby->safe_psql('postgres', 'UPDATE t SET v = v + 1');
	$standby->wait_for_catchup($primary);
	$expected =
	  $standby->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t');
	is($primary->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t'),
		$expected, "$am: rewound data matches the source");
	is( $primary->safe_psql(
			'postgres', "SELECT bt_index_check('t_pkey', true)"),
		'',
		"$am: index consistent after rewind");
	unlike(
		slurp_file($primary->logfile, $logstart),
		qr/unknown undo log/,
		"$am: no unknown undo log during replay");
	ok( $am eq 'heap' || $primary->log_contains(
			'undo log checkpoint snapshot for redo point .* is missing, using',
			$logstart),
		"$am: replay started from a newer undo log snapshot");
	$primary->stop;
	$standby->stop;

	# With every snapshot gone, a cluster that has undo logs must refuse to
	# start instead of starting with no undo logs.
	next if $am eq 'heap';
	my $dir = $primary->data_dir . '/pg_undo';
	unlink glob("$dir/*");
	command_fails_like(
		[ 'postgres', '-D', $primary->data_dir ],
		qr/undo log checkpoint snapshot .* is missing/,
		"$am: start without any undo log snapshot fails");
}
done_testing();
