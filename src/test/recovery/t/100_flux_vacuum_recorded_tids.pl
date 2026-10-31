# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX VACUUM Phase III frees a dead line pointer only if Phase I recorded
# it, so that Phase II removed the index entries naming it.  A row that dies
# between Phase I and Phase III stays LP_DEAD for the next VACUUM; were it
# set LP_UNUSED, the next INSERT would reuse a TID an index still names.
#
# The row that dies late is a committed DELETE that an older snapshot still
# saw at Phase I (recently dead, not recorded) and that opportunistic pruning
# marks LP_DEAD while VACUUM waits between Phase II and Phase III.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_vacuum_recorded');
$node->init;
$node->append_conf('postgresql.conf',
	"autovacuum = off\n" . "shared_preload_libraries = 'injection_points'\n");
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_visibility;
CREATE EXTENSION injection_points;
CREATE TABLE ab (id int, pad text) USING flux WITH (autovacuum_enabled = off);
CREATE INDEX ab_id ON ab (id);
INSERT INTO ab VALUES (0, repeat('x', 200));
CREATE FUNCTION release_one(p text) RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity WHERE wait_event = p) THEN
	RETURN true;
  END IF;
  BEGIN
	PERFORM injection_points_wakeup(p);
  EXCEPTION WHEN OTHERS THEN
	NULL;
  END;
  RETURN false;
END $$;
});

# lp_flags of line pointer $lp on block 0 of $t; 0 if absent, -1 if the
# relation has no block 0 (so truncation fails an assertion, not the test).
my $lp_flags = sub {
	my ($t, $lp) = @_;
	return -1
	  if $node->safe_psql('postgres', "SELECT pg_relation_size('$t')") == 0;
	return $node->safe_psql('postgres',
		"SELECT coalesce((SELECT lp_flags FROM heap_page_items(get_raw_page('$t', 0)) WHERE lp = $lp), 0)"
	);
};

# Fill page 0 so that opportunistic pruning runs on it (it only prunes a
# page with under 10% free space).  Row 1 is lp 2.
$node->safe_psql('postgres',
	"INSERT INTO ab SELECT g, repeat('x', 200) FROM generate_series(1, 34) g"
);
cmp_ok(
	$node->safe_psql(
		'postgres',
		"SELECT count(*) FROM heap_page_items(get_raw_page('ab', 0))"),
	'>=', 30,
	'page 0 is nearly full');

# An old snapshot keeps row 1 visible while it is deleted, so VACUUM's
# Phase I sees it recently dead and does not record it.
my $old = $node->background_psql('postgres');
$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$old->query_safe('SELECT count(*) FROM ab');
$node->safe_psql('postgres', 'DELETE FROM ab WHERE id = 1');

$node->safe_psql('postgres',
	"SELECT injection_points_attach('flux-vacuum-before-phase3', 'wait')");
my $vac = $node->background_psql('postgres');
$vac->query_until(qr/start/, "\\echo start\nVACUUM ab;\n");
$node->wait_for_event('client backend', 'flux-vacuum-before-phase3');

# The snapshot goes away; a seq scan prunes row 1 with a fresh horizon,
# leaving it LP_DEAD while its index entry, never recorded, survives.
$old->query_safe('COMMIT');
$old->quit;
$node->safe_psql('postgres', 'SELECT count(*) FROM ab');
is($lp_flags->('ab', 2), '3',
	'pruning left row 1 LP_DEAD while VACUUM waits');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('flux-vacuum-before-phase3')");
$node->poll_query_until('postgres',
	"SELECT release_one('flux-vacuum-before-phase3')")
  or die 'VACUUM was not released';
$vac->query_safe('SELECT 1');    # VACUUM has finished

isnt($lp_flags->('ab', 2),
	'0', 'VACUUM does not free a line pointer that Phase I did not record');

# The LP_DEAD slot left behind keeps the page out of the visibility map, so
# the next VACUUM visits the page and frees the slot, as heap's does.
is( $node->safe_psql(
		'postgres',
		"SELECT all_visible OR all_frozen FROM pg_visibility_map('ab', 0)"),
	'f',
	'a page with an LP_DEAD slot is not marked all-visible');
$node->safe_psql('postgres', 'VACUUM ab');
is($lp_flags->('ab', 2), '0', 'the next VACUUM frees the LP_DEAD slot');

# Reuse: the freed TID is reused only now that no index entry names it.
$node->safe_psql('postgres',
	"INSERT INTO ab SELECT g, 'y' FROM generate_series(100, 140) g");
is( $node->safe_psql(
		'postgres', q{SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ab WHERE id = 1}),
	'0',
	'the stale index entry for id 1 leads to no row');
is( $node->safe_psql(
		'postgres', q{SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ab}),
	$node->safe_psql(
		'postgres', q{SET enable_indexscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM ab}),
	'index scan and seq scan agree');
is($node->safe_psql('postgres', "SELECT bt_index_check('ab_id', true)"),
	'', 'bt_index_check heapallindexed passes');

$node->safe_psql('postgres', 'VACUUM ab');
is($node->safe_psql('postgres', "SELECT bt_index_check('ab_id', true)"),
	'', 'bt_index_check passes after a further VACUUM');
$vac->quit;
$node->stop;
done_testing();
