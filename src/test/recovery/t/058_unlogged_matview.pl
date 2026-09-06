# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests the crash-recovery contract for UNLOGGED MATERIALIZED VIEWs.
#
# An unlogged matview's pg_class.relpopulated carries an epoch stamp: the end
# of WAL at the last reset of unlogged relations before it was populated.
# Crash recovery starts a new epoch at its own end of WAL, so after a crash
# the stale stamp reads as unpopulated at scan time, with no catalog repair
# needed.  A clean restart leaves the epoch unchanged, so contents survive.
# A logged matview is unaffected by a crash.  REFRESH stamps the current
# epoch and restores the contents.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('umv');
$node->init;
$node->start;

# mv_conv is converted to LOGGED after the crash below.
$node->safe_psql('postgres', <<'SQL');
CREATE UNLOGGED MATERIALIZED VIEW mv_u AS SELECT 42 AS x;
CREATE UNLOGGED MATERIALIZED VIEW mv_conv AS SELECT 7 AS x;
CREATE MATERIALIZED VIEW mv_p AS SELECT 43 AS x;
SQL

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview is scannable after creation');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'unlogged matview reports populated after creation');

# --- Clean restart preserves contents -----------------------------------------
#
# A clean shutdown does not reset unlogged relations or start a new epoch, so
# the epoch stamp is still current and the data survives.

$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview contents preserved across a clean restart');

# --- Crash makes the epoch stamp stale ----------------------------------------
#
# Crash recovery starts a new epoch, so the stamp written at population time
# no longer matches the current epoch.  Reads treat the matview as
# unpopulated without any catalog write.

$node->stop('immediate');
$node->start;

my ($rc, $out, $err) =
  $node->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'SELECT on crash-stale unlogged matview fails');
like(
	$err,
	qr/has not been populated/,
	'crash-stale unlogged matview reports "has not been populated"');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false for crash-stale unlogged matview');
is( $node->safe_psql(
		'postgres',
		q{SELECT ispopulated FROM pg_matviews WHERE matviewname = 'mv_u'}),
	'f',
	'pg_matviews.ispopulated is false for crash-stale unlogged matview');

# The logged matview is unaffected by the crash.
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview still returns rows after crash');

# ALTER MATERIALIZED VIEW ... SET LOGGED must not launder a stale epoch stamp
# into an eternally-populated state: the unlogged storage was reset by the
# crash, so the converted matview must read as unpopulated until REFRESH.
$node->safe_psql('postgres', 'ALTER MATERIALIZED VIEW mv_conv SET LOGGED');
is( $node->safe_psql(
		'postgres',
		q{SELECT relpersistence FROM pg_class WHERE oid = 'mv_conv'::regclass}
	),
	'p',
	'crash-stale unlogged matview converted to LOGGED');
($rc, $out, $err) = $node->psql('postgres', 'SELECT count(*) FROM mv_conv');
like(
	$err,
	qr/has not been populated/,
	'stale stamp converted to "not populated", not to eternally-populated');

# WITH NO DATA clears the stale stamp, so pg_dump no longer sees it as
# meant to be populated.
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u WITH NO DATA');
is( $node->safe_psql(
		'postgres',
		q{SELECT relpopulated FROM pg_class WHERE oid = 'mv_u'::regclass}),
	'0',
	'REFRESH WITH NO DATA clears a stale stamp');

# REFRESH stamps the current epoch and restores the contents.
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_conv');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'REFRESH restores the unlogged matview after crash');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'unlogged matview reports populated again after REFRESH');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_conv'),
	'1', 'REFRESH restores the converted matview');

# --- pg_resetwal after a clean shutdown keeps the contents --------------------
#
# Resetting the WAL of a cleanly shut down cluster does not touch unlogged
# storage, and the next startup does not run crash recovery, so the epoch
# stamp must stay current.  pg_upgrade relies on this: it resets the WAL of
# the new cluster after restoring the schema, and the transferred unlogged
# matview heaps have to remain usable.

$node->stop;

$node->command_ok([ 'pg_resetwal', '-D', $node->data_dir ],
	'pg_resetwal on a cleanly shut down cluster');

$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview survives pg_resetwal after a clean shutdown');

# --- pg_resetwal after an unclean shutdown discards the contents --------------
#
# Here the unlogged storage is torn and nothing will reset it later, so the
# epoch has to move and the matview must read as unpopulated.

$node->stop('immediate');

$node->command_ok([ 'pg_resetwal', '-f', '-D', $node->data_dir ],
	'pg_resetwal -f on an uncleanly shut down cluster');

$node->start;

($rc, $out, $err) = $node->psql('postgres', 'SELECT count(*) FROM mv_u');
like(
	$err,
	qr/has not been populated/,
	'unlogged matview unpopulated after pg_resetwal -f on a dirty cluster');

done_testing();
