
# Copyright (c) 2026, PostgreSQL Global Development Group

# FLUX: VACUUM truncates an empty tail with heap's protocol.  A session
# holding ROW EXCLUSIVE makes the AccessExclusiveLock attempt give up, and
# VACUUM VERBOSE must then report no pages truncated; once the lock is free
# the whole empty tail goes and VERBOSE reports the pages it truncated.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_truncate');
$node->init;
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'\n")
  if ($ENV{enable_injection_points} // '') eq 'yes';
$node->start;

my $stderr;
$node->safe_psql(
	'postgres', q{
CREATE TABLE ft (id int, pad text) USING flux;
INSERT INTO ft SELECT g, repeat('t', 200) FROM generate_series(1, 20000) g;
DELETE FROM ft WHERE id > 1;
});
my $fpages =
  $node->safe_psql('postgres', q{SELECT pg_relation_size('ft') / 8192});
my $holder = $node->background_psql('postgres');
$holder->query_safe('BEGIN');
$holder->query_safe('LOCK ft IN ROW EXCLUSIVE MODE');
$node->psql('postgres', 'VACUUM VERBOSE ft', stderr => \$stderr);
like(
	$stderr,
	qr/stopping truncate due to conflicting lock request/,
	'FLUX: truncation gives up behind a conflicting lock');
like(
	$stderr,
	qr/truncated 0 pages/,
	'FLUX: VACUUM VERBOSE reports no truncation when it gave up');
is($node->safe_psql('postgres', q{SELECT pg_relation_size('ft') / 8192}),
	$fpages, 'FLUX: the table kept its pages');
$holder->query_safe('COMMIT');
$holder->quit;
$node->psql('postgres', 'VACUUM VERBOSE ft', stderr => \$stderr);
my $want = $fpages - 1;
like(
	$stderr,
	qr/truncated $want pages/,
	'FLUX: VACUUM VERBOSE reports the pages it truncated');
is($node->safe_psql('postgres', q{SELECT pg_relation_size('ft') / 8192}),
	'1', 'FLUX: empty tail truncated');

# Rows written into the empty tail after VACUUM found it, but before it
# truncates, must survive: the truncation gives up when the relation grew and
# otherwise recounts the tail under its AccessExclusiveLock.  Hold VACUUM
# there with an injection point and write as many rows as were deleted, so
# that they reach the last page whether they go to the free pages or extend
# the relation.
SKIP:
{
	skip 'injection points not supported by this build', 2
	  if ($ENV{enable_injection_points} // '') ne 'yes';
	skip 'Extension injection_points not installed', 2
	  if !$node->check_extension('injection_points');

	$node->safe_psql(
		'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE fr (id int, pad text) USING flux WITH (autovacuum_enabled = off);
INSERT INTO fr SELECT g, repeat('r', 200) FROM generate_series(1, 20000) g;
DELETE FROM fr WHERE id > 1;
SELECT injection_points_attach('flux-vacuum-before-truncate', 'wait');
});
	my $vacuumer = $node->background_psql('postgres');
	$vacuumer->query_until(
		qr/started/, q{
\echo started
VACUUM fr;
});
	$node->wait_for_event('client backend', 'flux-vacuum-before-truncate');
	$node->safe_psql(
		'postgres', q{
INSERT INTO fr SELECT g, repeat('w', 200) FROM generate_series(100001, 120000) g;
});
	$node->safe_psql(
		'postgres', q{
SELECT injection_points_wakeup('flux-vacuum-before-truncate');
SELECT injection_points_detach('flux-vacuum-before-truncate');
});
	$vacuumer->quit;

	is($node->safe_psql('postgres', q{SELECT count(*) FROM fr}),
		'20001', 'FLUX: rows written during VACUUM survive truncation');
	is( $node->safe_psql(
			'postgres',
			q{SELECT count(*) FROM fr WHERE id > 100000 AND pad = repeat('w', 200)}
		),
		'20000',
		'FLUX: their values are intact');
}

$node->stop;
done_testing();
