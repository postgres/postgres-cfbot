# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests the standby scannability contract for UNLOGGED MATERIALIZED VIEWs.
#
# Unlogged relations are never streamed to a standby, so an unlogged
# matview's storage does not exist there.  Its pg_class.relpopulated epoch
# stamp is replicated verbatim from the primary, but
# MatViewPopulatedValueIsValid() treats any epoch stamp as invalid whenever
# RecoveryInProgress() is true, so on a standby every unlogged matview reads
# as unpopulated regardless of what the primary thinks.  A SELECT, EXPLAIN,
# or COPY TO must therefore raise the standard "has not been populated" /
# "unpopulated materialized view" errors rather than silently returning zero
# rows or failing with the generic "cannot access temporary or unlogged
# relations during recovery" message.  A logged matview is fully replicated
# and remains scannable on the standby.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node with streaming replication enabled.
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql('postgres', <<'SQL');
CREATE UNLOGGED MATERIALIZED VIEW mv_u AS SELECT 42 AS x;
CREATE MATERIALIZED VIEW mv_p AS SELECT 42 AS x;
SQL

# Refresh the unlogged matview once more before backing up: REFRESH swaps in
# a new relfilenode whose main fork is populated via a bulk-insert path that
# is not WAL-logged for unlogged relations.  This exercises the plan-time
# guard's handling of a "current" epoch stamp whose main fork the standby
# never receives.
$node_primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');

# Both matviews are populated and scannable on the primary.
is($node_primary->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview is scannable on the primary');
is($node_primary->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview is scannable on the primary');

# Take a base backup and create a streaming standby.
$node_primary->backup('bkp');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'bkp', has_streaming => 1);
$node_standby->start;

$node_primary->wait_for_catchup($node_standby);

# The logged matview is fully replicated and remains scannable on the
# standby, returning the primary's data.
is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview is scannable on the standby (no regression)');

# The unlogged matview must report as unpopulated on the standby: not zero
# rows, and not the generic unlogged-relation-during-recovery error.
my ($rc, $out, $err) =
  $node_standby->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'SELECT on unlogged matview fails on standby');
like(
	$err,
	qr/has not been populated/,
	'unlogged matview reports "has not been populated" on standby');
unlike(
	$err,
	qr/cannot access temporary or unlogged relations during recovery/,
	'unlogged matview does not report the generic unlogged-relation error');

# The plan-time guard in plancat.c must reject it too, before execution.
($rc, $out, $err) =
  $node_standby->psql('postgres', 'EXPLAIN SELECT * FROM mv_u');
isnt($rc, 0, 'EXPLAIN on unlogged matview fails on standby');
like(
	$err,
	qr/has not been populated/,
	'EXPLAIN on unlogged matview reports "has not been populated" on standby'
);

# COPY TO must honor the same scannability contract.
($rc, $out, $err) = $node_standby->psql('postgres', 'COPY mv_u TO stdout');
isnt($rc, 0, 'COPY from unlogged matview on standby fails');
like(
	$err,
	qr/unpopulated materialized view/,
	'COPY from unlogged matview reports unpopulated error on standby');

# pg_matview_is_populated() must be accurate per-node: false on the standby
# while the very same matview reports true on the primary.
is( $node_standby->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false for unlogged matview on standby');
is( $node_primary->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'pg_matview_is_populated is still true for unlogged matview on primary');

# Likewise for pg_matviews.ispopulated.
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT ispopulated FROM pg_matviews WHERE matviewname = 'mv_u'}),
	'f',
	'pg_matviews.ispopulated is false for unlogged matview on standby');
is( $node_primary->safe_psql(
		'postgres',
		q{SELECT ispopulated FROM pg_matviews WHERE matviewname = 'mv_u'}),
	't',
	'pg_matviews.ispopulated is still true for unlogged matview on primary');

# --- Primary-side REFRESH does not change the standby's verdict ------------
#
# A fresh REFRESH on the primary writes a new "current" epoch stamp and
# replicates it to the standby, but the standby is still in recovery, so the
# stamp is still treated as invalid there.  This also exercises the
# missing-main-fork safety: the standby never received the new relfilenode's
# main fork contents (unlogged relfilenodes are not streamed), yet no code
# path attempts to actually read it.

$node_primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
$node_primary->wait_for_catchup($node_standby);

($rc, $out, $err) =
  $node_standby->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0,
	'SELECT on unlogged matview still fails on standby after primary REFRESH'
);
like(
	$err,
	qr/has not been populated/,
	'unlogged matview still reports "has not been populated" on standby after primary REFRESH'
);

# The primary itself is unaffected and remains populated.
is($node_primary->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'unlogged matview remains scannable on the primary after REFRESH');

$node_standby->stop;
$node_primary->stop;

done_testing();
