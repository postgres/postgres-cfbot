# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that an autovacuum worker's freeze score doesn't wrap around when a
# table's relfrozenxid advances after the worker has read recentXid.
#
# The worker reads recentXid before do_autovacuum() scans pg_class.  This
# test holds a worker at the autovacuum-worker-start injection point, which
# is in that window, freezes a table so that its relfrozenxid moves past the
# worker's recentXid, and then checks the xid score the worker computes for
# the table.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum_naptime = 1
log_min_messages = debug3
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql(
	'postgres', qq(
    CREATE EXTENSION injection_points;
    CREATE TABLE av_score_wrap (a int);
    INSERT INTO av_score_wrap VALUES (1);
));

# From this point, autovacuum workers wait after reading recentXid.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('autovacuum-worker-start', 'wait');");
$node->reload();

# Hold a worker for the postgres database.  Workers for other databases are
# let go.
my $av_pid;
for (my $i = 0; $i < 30; $i++)
{
	$node->wait_for_event('autovacuum worker', 'autovacuum-worker-start');
	$av_pid = $node->safe_psql(
		'postgres', qq(
        SELECT pid FROM pg_stat_activity
         WHERE backend_type = 'autovacuum worker'
           AND wait_event = 'autovacuum-worker-start'
           AND datname = 'postgres'
         LIMIT 1;
    ));
	last if $av_pid ne '';
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('autovacuum-worker-start');");
}
isnt($av_pid, '', 'autovacuum worker for postgres held at injection point');

# Move the table's relfrozenxid past the worker's recentXid.
$node->safe_psql('postgres', "SELECT pg_current_xact_id();") for (1 .. 5);
$node->safe_psql('postgres', 'VACUUM (FREEZE) av_score_wrap;');

my $offset = -s $node->logfile;

# Let the worker go on; it computes the scores in its pg_class scan.
$node->safe_psql(
	'postgres', qq(
    SELECT injection_points_detach('autovacuum-worker-start');
    SELECT injection_points_wakeup('autovacuum-worker-start');
));

$node->wait_for_log(qr/av_score_wrap: vac: .* xid score: /, $offset);

my $log = slurp_file($node->logfile, $offset);
my ($xid_score) = $log =~ /av_score_wrap: vac: .* xid score: ([0-9.e+]+)/;
note "xid score for av_score_wrap: $xid_score";
cmp_ok($xid_score, '<', 1.0,
	'xid score of a just-frozen table does not wrap around');

done_testing();
