# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that the autoprewarm worker gives up a relation when a conflicting
# lock request is waiting, letting the DDL proceed.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

# The worker only loads the dump at startup and injection points do not
# survive a restart, so the wait can only be armed after the restart below.
# A large table and buffer pool keep the scan running long enough to attach
# the injection point and still catch a later check. That size makes this a
# heavy, manual/local test rather than one for the buildfarm.
$node->append_conf(
	'postgresql.conf', qq{
shared_preload_libraries = 'pg_prewarm,injection_points'
pg_prewarm.autoprewarm = true
pg_prewarm.autoprewarm_interval = 0
autovacuum = off
shared_buffers = '2GB'
});
$node->start;

# The injection_points extension may not be installed under installcheck.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', q(
	CREATE EXTENSION pg_prewarm;
	CREATE EXTENSION injection_points;
));

$node->safe_psql('postgres', q(
	CREATE TABLE warm_tbl (id int, pad text);
	INSERT INTO warm_tbl SELECT g, repeat('x', 500)
		FROM generate_series(1, 1000000) g;
));

# The table must exceed the worker's lock-check interval (in blocks) so that
# the worker reaches the injection point while still scanning it.
my $nblocks = $node->safe_psql('postgres',
	"SELECT pg_relation_size('warm_tbl') / current_setting('block_size')::int");
ok($nblocks > 32, "table has more than 32 blocks ($nblocks)");

# Warm the table and record its blocks so the worker reloads them on restart.
$node->safe_psql('postgres', "SELECT pg_prewarm('warm_tbl', 'buffer')");
$node->safe_psql('postgres', "SELECT autoprewarm_dump_now()");

$node->restart;

# Pause the worker mid-prewarm, but only while it scans the target table.
# Other relations in the dump (some catalogs have more than 32 blocks) reach
# this point first, so without the condition the worker could stop holding a
# lock on the wrong relation and the TRUNCATE below would not block.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autoprewarm-before-lock-check', 'wait', 'warm_tbl')");
$node->wait_for_event('autoprewarm worker', 'autoprewarm-before-lock-check');

# TRUNCATE now blocks on the AccessExclusiveLock the worker conflicts with.
my $truncate = $node->background_psql('postgres');
$truncate->query_until(qr/starting_truncate/, q(
	\echo starting_truncate
	TRUNCATE warm_tbl;
));
$node->poll_query_until('postgres', q(
	SELECT count(*) > 0 FROM pg_stat_activity
	WHERE query LIKE '%TRUNCATE warm_tbl%' AND wait_event_type = 'Lock';
)) or die "timed out waiting for TRUNCATE to block on the lock";

# Resume the worker; it should see the waiter and release its lock.
my $log_offset = -s $node->logfile;
$node->safe_psql('postgres',
	"SELECT injection_points_detach('autoprewarm-before-lock-check')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('autoprewarm-before-lock-check')");

$truncate->quit;
pass('TRUNCATE completed while autoprewarm worker was prewarming');

# Having given up the table, the worker warmed fewer blocks than it dumped.
$node->wait_for_log(
	qr/autoprewarm successfully prewarmed \d+ of \d+ previously-loaded blocks/,
	$log_offset);
my $summary = slurp_file($node->logfile, $log_offset);
my ($prewarmed, $total) = $summary =~
	/successfully prewarmed (\d+) of (\d+) previously-loaded blocks/;
cmp_ok($prewarmed, '<', $total,
	"worker gave up early: prewarmed $prewarmed of $total blocks");

$node->stop;
done_testing();
