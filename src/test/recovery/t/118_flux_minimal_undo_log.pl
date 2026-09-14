# Copyright (c) 2026, PostgreSQL Global Development Group

# At wal_level = minimal a relation created in the same transaction skips
# WAL: it is synchronized at commit.  FLUX still writes undo for it, and that
# undo must not end up in an undo log that crash recovery replays, because
# nothing in the WAL describes it.  Two consequences are checked, with a heap
# control:
#
# - A later WAL-logged change whose undo lands in the same log must replay
#   after a crash.
# - Once the discard worker has discarded that undo, the discard is
#   WAL-logged; after a crash the log must still be consistent, the discard
#   worker must answer a shutdown request, and new undo must be writable.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $am (qw(heap flux))
{
	my $node = PostgreSQL::Test::Cluster->new("minimal_$am");
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq[
wal_level = minimal
max_wal_senders = 0
wal_skip_threshold = 0
autovacuum = off
]);
	$node->start;
	$node->safe_psql(
		'postgres', qq[
		CREATE TABLE old (id int PRIMARY KEY, v int) USING $am;
		INSERT INTO old SELECT g, 0 FROM generate_series(1, 100) g;
		CHECKPOINT;
	]);

	# One session: a transaction that skips WAL, then a WAL-logged one.
	$node->safe_psql(
		'postgres', qq[
		BEGIN;
		CREATE TABLE neu (id int, v int) USING $am;
		INSERT INTO neu SELECT g, g FROM generate_series(1, 5000) g;
		UPDATE neu SET v = v + 1;
		COMMIT;
		UPDATE old SET v = v + 1 WHERE id <= 50;
	]);
	$node->stop('immediate');

	my $ret = $node->start(fail_ok => 1);
	ok($ret,
		"$am: crash recovery replays a change logged after a transaction that skipped WAL"
	);
	if (!$ret)
	{
		my ($panic) = slurp_file($node->logfile) =~ /(PANIC:.*)/;
		diag($panic // 'no PANIC');
		next;
	}
	is( $node->safe_psql(
			'postgres',
			'SELECT sum(v) FROM old UNION ALL SELECT sum(v) FROM neu'),
		"50\n12507500",
		"$am: both transactions survive the crash");

	# Again, but let the discard worker discard the undo before the crash.
	$node->safe_psql(
		'postgres', qq[
		BEGIN;
		CREATE TABLE neu2 (id int, v int) USING $am;
		INSERT INTO neu2 SELECT g, g FROM generate_series(1, 5000) g;
		UPDATE neu2 SET v = v + 1;
		DELETE FROM neu2;
		COMMIT;
	]);
	ok( $node->poll_query_until(
			'postgres', q(
		SELECT coalesce(bool_and(discard = insert), true)
		  FROM pg_stat_get_undo_logs())),
		"$am: the discard worker discards the undo");
	$node->stop('immediate');
	$node->start;
	is($node->safe_psql('postgres', 'SELECT count(*) FROM neu2'),
		'0', "$am: committed delete of a table that skipped WAL survives");
	is( $node->safe_psql(
			'postgres', q(
		SELECT count(*) FROM pg_stat_get_undo_logs()
		 WHERE discard > insert)),
		'0',
		"$am: no undo log is discarded past its insert point after recovery");
	my ($uret, $uout, $uerr) =
	  $node->psql('postgres', 'UPDATE old SET v = v + 1 WHERE id <= 10');
	is($uret, 0, "$am: undo is writable after recovery") or diag $uerr;
	my $stopped = $node->stop('fast', fail_ok => 1);
	ok($stopped, "$am: fast shutdown completes after recovery");
	$node->stop('immediate') if !$stopped;
}

done_testing();
