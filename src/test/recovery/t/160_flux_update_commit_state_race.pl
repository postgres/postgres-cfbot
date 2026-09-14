
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# An UPDATE of a FLUX row whose last writer commits while the updater checks
# it must not lose that writer's update.
#
# FLUX's same-size in-place UPDATE (the CAS fast path) asks whether the row's
# newest version was written by a transaction its snapshot cannot see that has
# since committed (FluxTupleHasCommittedUpdateAfter).  It asked CLOG first and
# the proc array second.  A writer marks itself committed in CLOG before it
# leaves the proc array, so a writer that did both between the two questions
# read as "not committed" and "not in progress": no conflict, and the updater
# overwrote the committed row with a value computed from the version before
# it.  HammerDB TPROC-C hit this on a 128-core host (two neword calls handed
# out the same order id from district.d_next_o_id; the unique index on orders
# caught the second).  heapam_visibility.c explains why the proc array has to
# be asked first.
#
# The writer is held after its commit record, before CLOG
# (commit-before-clog-update), so its UNCOMMITTED hint is already clear and
# the updater takes the fast path.  The updater is held between the two
# questions (flux-head-commit-state) if it gets there; the writer is then let
# finish.  Heap is the control.  With the proc array asked first, the updater
# finds the writer in progress and waits for it, as heap's does.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if (($ENV{enable_injection_points} // '') ne 'yes')
{
	plan skip_all => 'injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_commit_state');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
shared_preload_libraries = 'injection_points'
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# Wait until $pid is held at an injection point or waits for a lock.
sub wait_parked
{
	my ($pid) = @_;
	my $state;
	$node->poll_query_until(
		'postgres', qq{
SELECT wait_event_type = 'Lock'
       OR wait_event = 'flux-head-commit-state'
FROM pg_stat_activity WHERE pid = $pid}) or die "updater did not park";
	return $node->safe_psql('postgres',
		"SELECT wait_event FROM pg_stat_activity WHERE pid = $pid");
}

foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE c_$am (id int PRIMARY KEY, n int NOT NULL) USING $am;
INSERT INTO c_$am VALUES (1, 1);
});

	my $w = $node->background_psql('postgres', on_error_stop => 0);
	my $u = $node->background_psql('postgres', on_error_stop => 0);
	my $upid = $u->query_safe('SELECT pg_backend_pid()');

	# The writer stops after its commit record, before CLOG.
	$w->query_safe(
		q{SELECT injection_points_set_local(),
		         injection_points_attach('commit-before-clog-update', 'wait')}
	);
	$w->query_until(qr/w-start/,
		"\\echo w-start\nUPDATE c_$am SET n = n + 1 WHERE id = 1;\n");
	$node->wait_for_event('client backend', 'commit-before-clog-update');

	# The updater's snapshot does not see the writer.
	$u->query_safe(
		q{SELECT injection_points_set_local(),
		         injection_points_attach('flux-head-commit-state', 'wait')});
	$u->query_until(qr/u-start/,
		"\\echo u-start\nUPDATE c_$am SET n = n + 1 WHERE id = 1 RETURNING n - 1 AS got;\n\\echo u-done\n"
	);
	my $where = wait_parked($upid);
	note "$am: updater parked at $where";

	# Let the writer commit and leave the proc array, then the updater go on.
	$node->safe_psql('postgres',
		q{SELECT injection_points_wakeup('commit-before-clog-update')});
	$w->query_until(qr/w-done/, "\\echo w-done\n");
	if ($where eq 'flux-head-commit-state')
	{
		$node->safe_psql('postgres',
			q{SELECT injection_points_wakeup('flux-head-commit-state')});
	}
	my $out = $u->query_until(qr/u-done/, '');
	$u->query_safe(
		q{SELECT injection_points_detach('flux-head-commit-state')});
	$w->query_safe(
		q{SELECT injection_points_detach('commit-before-clog-update')});
	$w->quit;
	$u->quit;

	like($out, qr/^2$/m,
		"$am: the updater saw the writer's committed value (got 2)");
	is($node->safe_psql('postgres', "SELECT n FROM c_$am WHERE id = 1"),
		'3', "$am: both increments survive");
}

done_testing();
