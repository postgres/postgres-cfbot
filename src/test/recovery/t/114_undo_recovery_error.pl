# Copyright (c) 2026, PostgreSQL Global Development Group
# An ERROR in startup undo must fail startup, leaving the chain for retry.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
plan skip_all => 'Injection points required'
  unless $ENV{enable_injection_points} eq 'yes';
my $node = PostgreSQL::Test::Cluster->new('undo_recovery_error');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
shared_preload_libraries = 'test_idxundo'
});
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE f (id int, v int) USING flux;
INSERT INTO f VALUES (1, 10);
CHECKPOINT;
});
my $s = $node->background_psql('postgres');
$s->query_safe('BEGIN; UPDATE f SET v = 16908801;');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$s->{run}->kill_kill;
$node->append_conf('postgresql.conf', 'test_idxundo.recovery_error = on');
ok(!$node->start(fail_ok => 1), 'undo ERROR fails startup');
like(
	slurp_file($node->logfile),
	qr/FATAL:.*test_idxundo: recovery error/,
	'startup reports FATAL');
# Stop the baseline if it incorrectly accepted connections.
$node->stop('immediate', fail_ok => 1);
$node->append_conf('postgresql.conf', 'test_idxundo.recovery_error = off');
$node->start;
ok( $node->poll_query_until(
		'postgres', q{
SELECT position('\x01020201'::bytea IN get_raw_page('f', 0)) = 0
}),
	'retry physically removes the aborted image');
is($node->safe_psql('postgres', 'SELECT v FROM f'),
	'10', 'committed value survives restart');
$node->stop;
done_testing();
