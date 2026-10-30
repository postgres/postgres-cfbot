# Copyright (c) 2026, PostgreSQL Global Development Group

# CLOG truncation removes old pg_xact segments.  If a checkpoint whose redo
# pointer is after the truncation record still recorded the old oldestXid,
# recovery from it would set oldestClogXid to that value, and status lookups
# of XIDs in the removed segments would fail instead of returning NULL.

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
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION xid_wraparound');
my $old_xid = $node->safe_psql('postgres', 'SELECT pg_current_xact_id()');

# Use up more than one pg_xact segment, and freeze all databases except one.
$node->safe_psql('postgres', 'SELECT consume_xids(1100000)');
$node->safe_psql('postgres',
	'ALTER DATABASE template0 WITH ALLOW_CONNECTIONS true');
$node->safe_psql($_, 'VACUUM (FREEZE)') for qw(template0 template1);

# The last vacuum truncates CLOG, and stops there.
my $vacuum = $node->background_psql('postgres', on_error_stop => 0);
$vacuum->query_safe(
	q(SELECT injection_points_set_local();
	  SELECT injection_points_attach('clog-truncate-after-unlink', 'wait');));
$vacuum->query_until(
	qr/starting_vacuum/, q(
	\echo starting_vacuum
	VACUUM (FREEZE);
));
$node->wait_for_event('client backend', 'clog-truncate-after-unlink');
ok(!-e $node->data_dir . '/pg_xact/0000', 'oldest pg_xact segment removed');

# This checkpoint's redo pointer is after the truncation record.
$node->safe_psql('postgres', 'CHECKPOINT');

$node->stop('immediate');
$vacuum->{run}->finish;
$node->start;

my ($ret, $stdout, $stderr) =
  $node->psql('postgres', qq(SELECT pg_xact_status('$old_xid'::xid8)));
is($ret, 0, 'status of truncated XID can be looked up after crash recovery');
is($stdout, '', 'status of truncated XID is NULL after crash recovery');

$node->stop;
done_testing();
