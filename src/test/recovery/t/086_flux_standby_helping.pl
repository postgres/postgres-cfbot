# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX helping (a writer reverting an aborted, not-yet-reverted version) is
# for writers on the primary only.  A hot standby must not change a page
# itself: every change has to arrive through the WAL, or the standby's page
# diverges from the primary's.  Its readers see the right row anyway, from
# CLOG and the version chain.  The SnapshotAny row fetch that helps on the
# primary (flux_tuple_fetch_row_version) is reachable on a standby through a
# TID scan's fetch; it must not help there.
#
# The undo apply worker is held at the injection point
# pbu-undo-worker-before-apply, so the rollback stays pending.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
wal_consistency_checking = all
shared_preload_libraries = 'injection_points'
));
$primary->start;
if (   !$primary->check_extension('injection_points')
	|| !$primary->check_extension('pageinspect'))
{
	plan skip_all => 'injection_points or pageinspect not installed';
}
$primary->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE TABLE f (id int PRIMARY KEY, v int) USING flux;
INSERT INTO f SELECT g, g * 10 FROM generate_series(1, 4) g;
SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');
));
$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;

$primary->safe_psql('postgres',
	'BEGIN; UPDATE f SET v = 16908801 WHERE id = 1; ROLLBACK;');
$primary->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
# An abort does not flush its WAL, so wait for the insert position: the flush
# position wait_for_replay_catchup uses can still be short of the UPDATE.
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));

my $on_page =
  q{SELECT position('\x01020201'::bytea IN get_raw_page('f', 0)) > 0};
my $page_lsn = q{SELECT lsn FROM page_header(get_raw_page('f', 0))};
is($primary->safe_psql('postgres', $on_page),
	't', 'primary: rollback still pending');
is($standby->safe_psql('postgres', $on_page), 't', 'standby: same page');

is($standby->safe_psql('postgres', 'SELECT v FROM f WHERE id = 1'),
	'10', 'standby reads the committed value');
my $lsn_before = $standby->safe_psql('postgres', $page_lsn);
my $ctid = $standby->safe_psql('postgres', 'SELECT ctid FROM f WHERE id = 1');
is($standby->safe_psql('postgres', "SELECT v FROM f WHERE ctid = '$ctid'"),
	'10', 'standby TID fetch returns the committed value');
is($standby->safe_psql('postgres', $on_page),
	't', 'standby did not revert the aborted version');
is($standby->safe_psql('postgres', $page_lsn),
	$lsn_before, 'standby did not change the page');

# A writer on the primary helps; the standby replays that.
is( $primary->safe_psql(
		'postgres', 'UPDATE f SET v = v + 1 WHERE id = 1 RETURNING v'),
	'11',
	'primary writer builds on the committed value');
$primary->safe_psql(
	'postgres',
	"SELECT injection_points_wakeup('pbu-undo-worker-before-apply');
	 SELECT injection_points_detach('pbu-undo-worker-before-apply');");
$primary->wait_for_catchup($standby, 'replay', $primary->lsn('insert'));
is($standby->safe_psql('postgres', 'SELECT v FROM f WHERE id = 1'),
	'11', 'standby replays the helped row');
is($standby->safe_psql('postgres', $on_page),
	'f', 'aborted image gone on the standby');
unlike(
	slurp_file($standby->logfile),
	qr/inconsistent page found|TRAP|PANIC/,
	'standby pages match the primary');

$standby->stop;
$primary->stop;
done_testing();
