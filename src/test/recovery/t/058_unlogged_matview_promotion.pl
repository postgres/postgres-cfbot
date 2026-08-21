# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Tests unlogged matview validity across standby promotion.
#
# A populated unlogged matview's pg_class.relpopulated holds the epoch stamp
# (TimeLineID << 32) | unloggedResetGen of the node that populated it, and
# the stamp only reads as populated when it equals the reading node's current
# epoch.  Promotion switches the timeline, so every stamp replicated from the
# old primary instantly reads as unpopulated on the promoted node, with no
# catalog write, no reconcile step, and no reconnect requirement.
#
# The choreography below deliberately constructs a generation-counter
# collision to prove that the timeline half of the stamp is load-bearing: the
# primary crashes once BEFORE the base backup (its generation becomes 1, and
# the backup carries generation 1 into the standby's pg_control) and a second
# time AFTER the backup (generation 2), then refreshes the matview so the
# replicated stamp is (tli 1, gen 2).  When the standby promotes it bumps its
# own generation 1->2 and moves to timeline 2, so the stamp's generation
# numerically EQUALS the promoted node's current generation and only the
# timeline distinguishes them.  A design comparing generations alone would
# wrongly consider the matview populated over storage that was never
# replicated.
#
# The test also keeps one psql session connected across the promotion:
# because validity is recomputed from the current epoch at every scan, the
# surviving session must start reporting "has not been populated" as soon as
# recovery ends, rather than silently returning zero rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Extract "Unlogged reset generation" from a node's pg_controldata output.
sub unlogged_reset_gen
{
	my ($node) = @_;
	my ($stdout, $stderr) = run_command([ 'pg_controldata', $node->data_dir ]);
	$stdout =~ /^Unlogged reset generation:\s+(\d+)\r?$/m
	  or die "no unlogged reset generation in pg_controldata output";
	return $1;
}

# --- Primary, crashed once so its generation counter is 1 ------------------

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;

$node_primary->safe_psql('postgres',
	'CREATE UNLOGGED MATERIALIZED VIEW mv_u AS SELECT 42 AS x');

$node_primary->stop('immediate');
$node_primary->start;

is(unlogged_reset_gen($node_primary),
	'1', 'first crash bumped the primary unlogged reset generation to 1');

my ($rc, $out, $err) =
  $node_primary->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'unlogged matview is unpopulated on the primary after crash');
like(
	$err,
	qr/has not been populated/,
	'crash-stale unlogged matview reports "has not been populated"');

# Repopulate: the stamp is now (tli 1, gen 1).
$node_primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');

# --- Standby whose base backup carries local generation 1 ------------------

$node_primary->backup('bkp');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'bkp', has_streaming => 1);
$node_standby->start;

$node_primary->wait_for_catchup($node_standby);

# --- Second primary crash drifts the replicated stamp to generation 2 ------

$node_primary->stop('immediate');
$node_primary->start;

is(unlogged_reset_gen($node_primary),
	'2', 'second crash bumped the primary unlogged reset generation to 2');

# Repopulate again: the stamp is now (tli 1, gen 2), and it replicates to the
# standby, whose own pg_control still says generation 1.
$node_primary->safe_psql('postgres', 'REFRESH MATERIALIZED VIEW mv_u');
$node_primary->wait_for_catchup($node_standby);

my $stamp_sql = q{SELECT relpopulated FROM pg_class WHERE relname = 'mv_u'};
my $stamp_primary = $node_primary->safe_psql('postgres', $stamp_sql);
my $stamp_standby = $node_standby->safe_psql('postgres', $stamp_sql);
is($stamp_standby, $stamp_primary,
	'standby replicated the primary\'s new epoch stamp');
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT relpopulated >> 32, relpopulated & 4294967295
		  FROM pg_class WHERE relname = 'mv_u'}),
	'1|2',
	'replicated stamp carries timeline 1, generation 2');

# --- A session that will survive the promotion -----------------------------

my $bg = $node_standby->background_psql('postgres', on_error_stop => 0);

is($bg->query_safe('SELECT pg_is_in_recovery()'),
	't', 'surviving session is connected to the standby before promotion');

# On the standby the unlogged matview is unpopulated (in-recovery rule).
my ($bg_out, $bg_err) = $bg->query('SELECT count(*) FROM mv_u');
is($bg_err, 1,
	'surviving session: unlogged matview errors on the standby');
like(
	$bg->{stderr},
	qr/has not been populated/,
	'surviving session: standby reports "has not been populated"');
$bg->{stderr} = '';

# --- Promote ----------------------------------------------------------------

$node_standby->promote;
$node_standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "standby never left recovery after promotion";

# --- The surviving session must see the matview as unpopulated -------------
#
# This is the heart of the test: the session predates the promotion, so no
# connect-time repair could have run for it.  If the epoch check were not
# recomputed at scan time, the SELECT would silently return a zero count over
# the empty, never-replicated storage.

is($bg->query_safe('SELECT pg_is_in_recovery()'),
	'f', 'surviving session survived the promotion');

($bg_out, $bg_err) = $bg->query('SELECT count(*) FROM mv_u');
is($bg_err, 1,
	'surviving session: SELECT on unlogged matview errors after promotion');
isnt($bg_out, '0',
	'surviving session: SELECT did not silently return a zero count');
is($bg_out, '', 'surviving session: SELECT returned no rows at all');
like(
	$bg->{stderr},
	qr/has not been populated/,
	'surviving session: promoted node reports "has not been populated"');
$bg->{stderr} = '';

# --- A new connection agrees, and the collision arithmetic holds -----------

($rc, $out, $err) =
  $node_standby->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'new connection: SELECT on unlogged matview fails after promotion');
like(
	$err,
	qr/has not been populated/,
	'new connection: promoted node reports "has not been populated"');
is( $node_standby->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	'f',
	'pg_matview_is_populated is false on the promoted node');

# Verify the collision was constructed as intended: promotion bumped the
# node's generation to 2, numerically equal to the stamp's generation, so
# only the timeline (stamp 1 vs node 2) marks the stamp as stale.
$node_standby->safe_psql('postgres', 'CHECKPOINT');
is(unlogged_reset_gen($node_standby),
	'2', 'promotion bumped the standby unlogged reset generation to 2');
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT relpopulated & 4294967295 FROM pg_class WHERE relname = 'mv_u'}
	),
	'2',
	'stamp generation numerically equals the promoted node\'s generation');
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT relpopulated >> 32 FROM pg_class WHERE relname = 'mv_u'}),
	'1',
	'stamp timeline is still 1');
is( $node_standby->safe_psql(
		'postgres', 'SELECT timeline_id FROM pg_control_checkpoint()'),
	'2', 'promoted node is on timeline 2');

# --- REFRESH on the promoted node restores the matview ---------------------

$bg->query_safe('REFRESH MATERIALIZED VIEW mv_u');

is($bg->query_safe('SELECT count(*) FROM mv_u'),
	'1', 'surviving session: REFRESH restored the matview');
is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'new connection sees the refreshed matview');
is( $node_standby->safe_psql(
		'postgres', q{SELECT pg_matview_is_populated('mv_u'::regclass)}),
	't',
	'pg_matview_is_populated is true again after REFRESH');
is( $node_standby->safe_psql(
		'postgres',
		q{SELECT relpopulated >> 32, relpopulated & 4294967295
		  FROM pg_class WHERE relname = 'mv_u'}),
	'2|2',
	'post-promotion REFRESH stamped the current epoch (tli 2, gen 2)');

# One more, later, new connection: nothing (such as a v1-style late
# reconciler) may clobber the post-promotion refresh.
is($node_standby->safe_psql('postgres', 'SELECT count(*) FROM mv_u'),
	'1', 'a later new connection still sees the refreshed matview');

$bg->quit;

# --- Crashing the promoted node makes it unpopulated again ------------------

$node_standby->stop('immediate');
$node_standby->start;

is(unlogged_reset_gen($node_standby),
	'3', 'crash bumped the promoted node\'s unlogged reset generation to 3');

($rc, $out, $err) =
  $node_standby->psql('postgres', 'SELECT count(*) FROM mv_u');
isnt($rc, 0, 'unlogged matview is unpopulated after crashing the promoted node');
like(
	$err,
	qr/has not been populated/,
	'crash on the promoted node reports "has not been populated" again');

$node_standby->stop;
$node_primary->stop;

done_testing();
