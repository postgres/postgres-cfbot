# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests the crash-recovery contract for UNLOGGED MATERIALIZED VIEWs.
#
# An unlogged matview's pg_class.relpopulated carries an epoch stamp of the
# (timeline, unlogged-reset-generation) it was populated in.  Crash recovery
# bumps the generation, so after a crash the stale stamp reads as unpopulated
# at scan time, with no catalog repair needed.  A clean restart leaves the
# epoch unchanged, so contents survive.  A logged matview is unaffected by a
# crash.  REFRESH stamps the current epoch and restores the contents.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('umv');
$node->init;
$node->start;

$node->safe_psql('postgres', <<'SQL');
CREATE UNLOGGED MATERIALIZED VIEW mv_u AS SELECT 42 AS x;
CREATE MATERIALIZED VIEW mv_p AS SELECT 43 AS x;
SQL

# Both matviews are populated and scannable right after creation.
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview is scannable after creation');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview is scannable after creation');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'unlogged matview reports populated after creation');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_p'::regclass)}),
	't',
	'logged matview reports populated after creation');

# --- Clean restart preserves contents (negative control) -------------------
#
# A clean shutdown does not change the timeline or the unlogged-reset
# generation, so the epoch stamp is still current and the data survives.

$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview contents preserved across a clean restart');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'unlogged matview still populated after a clean restart');

# --- Crash makes the epoch stamp stale --------------------------------------
#
# Crash recovery bumps the unlogged-reset generation, so the stamp written at
# population time no longer matches the current epoch.  Reads treat the
# matview as unpopulated without any catalog write.

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
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_p'::regclass)}),
	't',
	'logged matview still populated after crash');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview still returns rows after crash');

# REFRESH stamps the current epoch and restores the contents.
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'REFRESH restores the unlogged matview after crash');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'unlogged matview reports populated again after REFRESH');

# --- A second crash moves the epoch again -----------------------------------

$node->stop('immediate');
$node->start;

($rc, $out, $err) = $node->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'SELECT on unlogged matview fails after second crash');
like(
	$err,
	qr/has not been populated/,
	'unlogged matview unpopulated again after second crash');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false after second crash');

# REFRESH again, then a clean restart: the new stamp stays current.
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
$node->restart;

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'refreshed unlogged matview survives a clean restart');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'refreshed unlogged matview still populated after clean restart');

# --- SET LOGGED converts a stale stamp to "not populated" -------------------
#
# ALTER MATERIALIZED VIEW ... SET LOGGED must not launder a stale epoch stamp
# into an eternally-populated state: the unlogged storage was reset by the
# crash, so the converted matview must read as unpopulated until REFRESH.

$node->safe_psql('postgres',
	'CREATE UNLOGGED MATERIALIZED VIEW mv_conv AS SELECT 7 AS x');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_conv'),
	'1', 'second unlogged matview is scannable after creation');

$node->stop('immediate');
$node->start;

$node->safe_psql('postgres', 'ALTER MATERIALIZED VIEW mv_conv SET LOGGED');
is( $node->safe_psql(
		'postgres',
		q{SELECT relpersistence FROM pg_class WHERE oid = 'mv_conv'::regclass}
	),
	'p',
	'crash-stale unlogged matview converted to LOGGED');

($rc, $out, $err) = $node->psql('postgres', 'SELECT count(*) FROM mv_conv');
isnt($rc, 0, 'SELECT on converted crash-stale matview fails');
like(
	$err,
	qr/has not been populated/,
	'stale stamp converted to "not populated", not to eternally-populated');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_conv'::regclass)}),
	'f',
	'pg_matview_is_populated is false after stale-stamp conversion');

# REFRESH repopulates it; being LOGGED now, it survives a crash.
$node->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_conv');
is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_conv'),
	'1', 'REFRESH restores the converted matview');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_conv'::regclass)}),
	't',
	'converted matview reports populated after REFRESH');

$node->stop('immediate');
$node->start;

is($node->safe_psql('postgres', 'SELECT count(*) FROM mv_conv'),
	'1', 'converted LOGGED matview survives a crash');
is( $node->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_conv'::regclass)}),
	't',
	'converted LOGGED matview still populated after a crash');

done_testing();
