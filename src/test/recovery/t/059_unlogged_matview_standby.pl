# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests UNLOGGED MATERIALIZED VIEWs on a standby, and when it leaves
# recovery.
#
# A standby never has unlogged storage, so an unlogged matview must read as
# unpopulated there, whatever epoch stamp the primary replicated.  Once the
# standby leaves recovery, its own epoch is past every replicated stamp, so
# the matview stays unpopulated without any catalog write.  Two standbys
# from one backup leave recovery differently: promotion (new timeline) and
# restart without standby.signal (same timeline).  A session kept open
# across the promotion must see the change at its next scan.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Extract "Unlogged relations reset at" from pg_controldata output.
sub unlogged_reset_lsn
{
	my ($node) = @_;
	my ($stdout, $stderr) = run_command([ 'pg_controldata', $node->data_dir ]);
	$stdout =~ /^Unlogged relations reset at:\s+(\S+)\r?$/m
	  or die "no unlogged reset LSN in pg_controldata output";
	return $1;
}

# relpopulated of mv_u compared with an LSN: returns "<", "=" or ">".
sub stamp_vs_lsn
{
	my ($node, $lsn) = @_;
	return $node->safe_psql(
		'postgres', qq{
		SELECT CASE sign(relpopulated - ('$lsn'::pg_lsn - '0/0'))
			   WHEN -1 THEN '<' WHEN 0 THEN '=' ELSE '>' END
		FROM pg_class WHERE relname = 'mv_u'});
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql('postgres', <<'SQL');
CREATE UNLOGGED MATERIALIZED VIEW mv_u AS SELECT 42 AS x;
CREATE MATERIALIZED VIEW mv_p AS SELECT 42 AS x;
SQL

# --- Two standbys from the same backup --------------------------------------

$node_primary->backup('bkp');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'bkp', has_streaming => 1);
$node_standby->start;

my $node_standby2 = PostgreSQL::Test::Cluster->new('standby2');
$node_standby2->init_from_backup($node_primary, 'bkp', has_streaming => 1);
$node_standby2->start;

# --- Primary crash and a fresh stamp ------------------------------------------

$node_primary->stop('immediate');
$node_primary->start;

my $reset_primary = unlogged_reset_lsn($node_primary);

# Repopulate, and let the new stamp replicate to both standbys, whose own
# pg_control still carries the epoch from the backup.
$node_primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
$node_primary->wait_for_catchup($node_standby);
$node_primary->wait_for_catchup($node_standby2);

is(stamp_vs_lsn($node_standby, $reset_primary),
	'=', 'standby replicated the primary\'s post-crash epoch stamp');

# --- In recovery, the unlogged matview is unpopulated -------------------------

is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM mv_p'),
	'1', 'logged matview is scannable on the standby');

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
like(
	$err,
	qr/has not been populated/,
	'EXPLAIN on unlogged matview reports "has not been populated" on standby'
);

# COPY TO must honor the same scannability contract.
($rc, $out, $err) = $node_standby->psql('postgres', 'COPY mv_u TO stdout');
like(
	$err,
	qr/unpopulated materialized view/,
	'COPY from unlogged matview reports unpopulated error on standby');

# pg_matview_is_populated() is per node: false on the standby while the very
# same stamp reads true on the primary.
is( $node_standby->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false for unlogged matview on standby');
is( $node_primary->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'pg_matview_is_populated is true for unlogged matview on primary');
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT ispopulated FROM pg_matviews WHERE matviewname = 'mv_u'}),
	'f',
	'pg_matviews.ispopulated is false for unlogged matview on standby');

# --- A session that will survive the promotion --------------------------------

my $bg = $node_standby->background_psql('postgres', on_error_stop => 0);

my ($bg_out, $bg_err) = $bg->query('SELECT count(*) FROM mv_u');
is($bg_err, 1, 'surviving session: unlogged matview errors on the standby');
$bg->{stderr} = '';

# --- Promote ------------------------------------------------------------------

$node_standby->promote;
$node_standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "standby never left recovery after promotion";

# The session predates the promotion, so no connect-time repair could have
# run for it.  If the epoch check were not recomputed at scan time, the
# SELECT would silently return a zero count over the empty, never-replicated
# storage.
is($bg->query_safe('SELECT pg_is_in_recovery()'),
	'f', 'surviving session survived the promotion');

($bg_out, $bg_err) = $bg->query('SELECT count(*) FROM mv_u');
is($bg_err, 1,
	'surviving session: SELECT on unlogged matview errors after promotion');
is($bg_out, '', 'surviving session: SELECT returned no rows at all');
like(
	$bg->{stderr},
	qr/has not been populated/,
	'surviving session: promoted node reports "has not been populated"');
$bg->{stderr} = '';

# A new connection agrees, and the promoted node reset unlogged relations
# past the replicated stamp.
is( $node_standby->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false on the promoted node');
my $reset_promoted = unlogged_reset_lsn($node_standby);
is(stamp_vs_lsn($node_standby, $reset_promoted),
	'<', 'replicated stamp is below the promoted node\'s epoch');

# REFRESH on the promoted node restores the matview.
$bg->query_safe('REFRESH MATERIALIZED VIEW mv_u');
is($bg->query_safe('SELECT count(*) FROM mv_u'),
	'1', 'surviving session: REFRESH restored the matview');
is(stamp_vs_lsn($node_standby, $reset_promoted),
	'=', 'post-promotion REFRESH stamped the promoted node\'s epoch');

$bg->quit;

# --- Leaving recovery without promotion ---------------------------------------
#
# Restart the second standby without standby.signal.  That runs plain crash
# recovery, which keeps timeline 1, yet the stamp it replicated from the
# primary must still read as unpopulated: the standby has no data for it.

$node_standby2->stop;
unlink($node_standby2->data_dir . '/standby.signal')
  or die "could not remove standby.signal: $!";
$node_standby2->start;

is( $node_standby2->safe_psql(
		'postgres', 'SELECT timeline_id FROM pg_control_checkpoint()'),
	'1', 'second standby left recovery on timeline 1');
is(stamp_vs_lsn($node_standby2, unlogged_reset_lsn($node_standby2)),
	'<', 'replicated stamp is below the second standby\'s epoch');

($rc, $out, $err) =
  $node_standby2->psql('postgres', 'SELECT count(*) FROM mv_u');
like(
	$err,
	qr/has not been populated/,
	'second standby reports "has not been populated"');

$node_standby2->stop;
$node_standby->stop;
$node_primary->stop;

done_testing();
