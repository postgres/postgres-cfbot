# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A rollback handed to an undo apply worker must finish, and its undo must
# become discardable, even when there is nothing left to undo or when some of
# it can never be applied.
#
# - The table was dropped (or truncated) before the worker got to it: the
#   records have nothing left to undo, so the request completes.
# - A record keeps failing (injection point pbu-undo-apply-skip): after a
#   bounded number of attempts the worker gives up with a WARNING instead of
#   retrying forever and pinning the undo log.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_worker_done');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\nundo_rollback_overflow_size = 0\nlog_min_messages = debug1\n"
);
$node->start;

# The worker logs "Undo action pushed(error)" (DEBUG1) whenever it puts a
# request back for a retry.
sub retries
{
	my ($from) = @_;
	my $log = substr(slurp_file($node->logfile), $from);
	return scalar(() = $log =~ /Undo action pushed\(error\)/g);
}

# Undo of a transaction is discardable once its rollback is done: the
# discard worker then moves past it.
sub wait_discarded
{
	my ($lsn, $what) = @_;
	ok( $node->poll_query_until(
			'postgres', qq(
SELECT count(*) > 0 FROM pg_get_wal_records_info('$lsn', pg_current_wal_lsn())
 WHERE resource_manager = 'UndoLog' AND record_type = 'DISCARD')),
		"$what: undo discarded");
}

$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');

foreach my $am ('flux')
{
	foreach my $op ('DROP TABLE', 'TRUNCATE')
	{
		my $t = "w_${am}_" . lc((split(/ /, $op))[0]);
		my $what = "$am, $op before the worker ran";
		$node->safe_psql(
			'postgres', qq(
CREATE TABLE $t (id int, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 10) g;
));
		my $lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
		my $off = -s $node->logfile;

		# The DROP / TRUNCATE takes the table's AccessExclusiveLock right after
		# the rollback, before the worker can open the relation; the worker's
		# open then waits and finds the relation gone or empty.
		$node->safe_psql(
			'postgres', qq(
BEGIN;
UPDATE $t SET v = 'x';
ROLLBACK;
$op $t;
));
		ok( $node->poll_query_until(
				'postgres', q{SELECT count(*) = 0 FROM pg_stat_activity
				  WHERE backend_type = 'undo apply worker'}),
			"$what: worker finished");
		wait_discarded($lsn, $what);

		is(retries($off), 0, "$what: request was not retried");
	}
}

SKIP:
{
	skip 'Injection points not supported by this build', 3
	  if $ENV{enable_injection_points} ne 'yes';

	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	$node->safe_psql(
		'postgres', q(
CREATE TABLE stuck (id int, v text) USING flux;
INSERT INTO stuck SELECT g, 'orig' || g FROM generate_series(1, 10) g;
));
	my $lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
	my $off = -s $node->logfile;
	$node->safe_psql(
		'postgres', q(
SELECT injection_points_attach('pbu-undo-apply-skip', 'notice');
BEGIN;
UPDATE stuck SET v = 'x';
ROLLBACK;
));
	ok( $node->wait_for_log(
			qr/WARNING:  .*giving up on rolling back transaction/, $off),
		'worker gives up on a request that keeps failing');
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-apply-skip')");
	my $n = retries($off);
	ok($n >= 1 && $n <= 10, "bounded number of retries ($n)");
	wait_discarded($lsn, 'given-up request');
}

$node->stop;
done_testing();
