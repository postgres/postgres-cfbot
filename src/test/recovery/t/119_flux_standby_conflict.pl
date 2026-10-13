
# Copyright (c) 2026, PostgreSQL Global Development Group

# Removing tuples on the primary must resolve recovery conflicts on a hot
# standby, as heap pruning does.  A standby query whose snapshot can still see
# the removed rows has to be cancelled before the removal is replayed: it must
# never read the page with the rows gone.  Replay that compacts a page also
# needs a cleanup lock, so a standby backend holding a pin on that page is
# cancelled with a buffer pin conflict.  heap is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq[
autovacuum = off
]);
$primary->start;
$primary->backup('bk');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf', qq[
hot_standby_feedback = off
max_standby_streaming_delay = 50ms
]);
$standby->start;

# Poll a condition for a bounded time, so a missing conflict fails fast.
sub poll_true
{
	my ($cond) = @_;

	for (my $i = 0; $i < 300; $i++)
	{
		return 1 if $cond->();
		usleep(100_000);
	}
	return 0;
}

sub catchup
{
	# VACUUM's last record need not be flushed yet: wait for the insert LSN.
	$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
}

sub conflicts
{
	my ($db, $kind) = @_;

	return $standby->safe_psql($db,
		"SELECT confl_$kind FROM pg_stat_database_conflicts WHERE datname = '$db'"
	);
}

# Open a REPEATABLE READ transaction on the standby that reads every row of
# table $tab, run $removal on the primary, then read again in the same
# transaction.  It must either have been cancelled or still see every row.
sub snapshot_case
{
	my ($am, $db, $tab, $what, $removal) = @_;

	$primary->safe_psql(
		$db, qq[
CREATE TABLE $tab (id int, v int, pad char(200)) USING $am
	WITH (fillfactor = 100);
INSERT INTO $tab SELECT g, g, 'x' FROM generate_series(1, 100) g;
]);
	catchup();

	my $s = $standby->background_psql($db, on_error_stop => 0);
	is( $s->query_safe(
			"BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*), sum(v) FROM $tab;"
		),
		'100|5050',
		"$am $what: standby snapshot sees every row");

	$primary->safe_psql($db, "DELETE FROM $tab WHERE id % 2 = 0;");
	$primary->safe_psql($db, $_) for @$removal;
	catchup();

	my $out = '';
	eval { $out = $s->query("SELECT count(*), sum(v) FROM $tab;"); };
	my $err = $s->{stderr} // '';
	ok( $err =~ /conflict with recovery/ || $out eq '100|5050',
		"$am $what: standby query cancelled or still sees every row (got '$out')"
	);
	eval { $s->quit; };
	return $err =~ /conflict with recovery/;
}

for my $am (qw(heap flux))
{
	my $db = "db_$am";

	$primary->safe_psql('postgres', "CREATE DATABASE $db");
	catchup();

	## (a) VACUUM removes rows a standby snapshot still sees.
	snapshot_case($am, $db, 'vac', 'VACUUM', ['VACUUM vac;']);
	ok(poll_true(sub { conflicts($db, 'snapshot') >= 1 }),
		"$am VACUUM: snapshot conflict counted");

	## (b) On-access pruning: a scan of full pages with deleted rows.
	snapshot_case(
		$am, $db, 'prn',
		'on-access pruning',
		[ 'SELECT count(*) FROM prn;', 'SELECT count(*) FROM prn;' ]);

	## (c) Buffer pin: replay of the removal needs a cleanup lock.
	# The removed row is an aborted insert older than the standby snapshot,
	# so it is not a snapshot conflict.
	$primary->safe_psql(
		$db, qq[
CREATE TABLE pin (a int, b int) USING $am;
INSERT INTO pin SELECT i % 3, 0 FROM generate_series(1, 20) i;
BEGIN;
INSERT INTO pin VALUES (1, 0);
ROLLBACK;
BEGIN; LOCK pin; COMMIT;
]);
	catchup();

	my $s = $standby->background_psql($db, on_error_stop => 0);
	like(
		$s->query_safe(
			"BEGIN; DECLARE c CURSOR FOR SELECT b FROM pin; FETCH FORWARD FROM c;"
		),
		qr/^0$/m,
		"$am buffer pin: cursor holds a pin on the page");
	my $log_offset = -s $standby->logfile;
	$primary->safe_psql($db, 'VACUUM pin;');
	catchup();
	ok( poll_true(
			sub {
				$standby->log_contains(
					qr/User was holding shared buffer pin for too long/,
					$log_offset);
			}),
		"$am buffer pin: conflict logged");
	ok(poll_true(sub { conflicts($db, 'bufferpin') >= 1 }),
		"$am buffer pin: conflict counted");
	eval { $s->quit; };

	is($standby->safe_psql($db, 'SELECT count(*) FROM pin'),
		'20', "$am buffer pin: standby reads the table after replay");
}

$standby->stop;
$primary->stop;
done_testing();
