use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$publisher->start;
$subscriber->start;

plan skip_all => 'injection points not supported by this build'
  unless $subscriber->check_extension('injection_points');
$subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points');

my $connstr = $publisher->connstr . ' dbname=postgres';
my $schema = q{
CREATE TABLE tab_first (a int PRIMARY KEY);
CREATE TABLE tab_second (a int PRIMARY KEY);
CREATE TABLE tab_sync (a int PRIMARY KEY);
};
my $subscription_lock = q{
locks.classid = 'pg_subscription'::regclass
AND locks.objid = (SELECT oid FROM pg_subscription WHERE subname = 'sub_sync')};

sub wait_for_lock
{
	my ($backend, $type, $target) = @_;
	$subscriber->poll_query_until('postgres', qq{
SELECT count(*) = 1 FROM pg_locks locks
JOIN pg_stat_activity activity USING (pid)
WHERE activity.backend_type = '$backend' AND NOT locks.granted
  AND locks.locktype = '$type' AND $target;
}) or die "$backend did not wait for $type lock";
}

$publisher->safe_psql('postgres', $schema . q{
INSERT INTO tab_sync VALUES (1);
CREATE PUBLICATION pub_sync FOR TABLE tab_first, tab_second;
});
$subscriber->safe_psql('postgres', $schema);
$subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION sub_sync CONNECTION '$connstr' PUBLICATION pub_sync WITH (disable_on_error = true)");
$subscriber->wait_for_subscription_sync($publisher, 'sub_sync');

my $sync_blocker = $subscriber->background_psql('postgres');
$sync_blocker->query_safe('BEGIN; LOCK TABLE tab_sync IN SHARE MODE');
$publisher->safe_psql('postgres', 'ALTER PUBLICATION pub_sync ADD TABLE tab_sync');
$subscriber->safe_psql('postgres', 'ALTER SUBSCRIPTION sub_sync REFRESH PUBLICATION');
wait_for_lock('logical replication tablesync worker', 'relation',
"locks.relation = 'tab_sync'::regclass");

my $first_blocker = $subscriber->background_psql('postgres');
$first_blocker->query_safe('BEGIN; LOCK TABLE tab_first IN SHARE MODE');
my $second_blocker = $subscriber->background_psql('postgres');
$second_blocker->query_safe('BEGIN; LOCK TABLE tab_second IN SHARE MODE');
$publisher->safe_psql('postgres', 'INSERT INTO tab_first VALUES (1)');
$publisher->safe_psql('postgres', 'INSERT INTO tab_second VALUES (1)');
wait_for_lock('logical replication apply worker', 'relation',
"locks.relation = 'tab_first'::regclass");
my $apply_pid = $subscriber->safe_psql('postgres', q{
SELECT pid FROM pg_stat_subscription WHERE subname = 'sub_sync' AND worker_type = 'apply';
});

$sync_blocker->query_safe('COMMIT');
$subscriber->poll_query_until('postgres', q{
SELECT count(*) = 1 FROM pg_stat_activity activity, pg_subscription_rel rel
WHERE activity.backend_type = 'logical replication tablesync worker'
  AND activity.wait_event = 'LogicalSyncStateChange'
  AND rel.srrelid = 'tab_sync'::regclass AND rel.srsubstate = 'f';
}) or die 'tablesync did not reach SYNCWAIT';
$first_blocker->query_safe('COMMIT');
wait_for_lock('logical replication apply worker', 'relation',
"locks.relation = 'tab_second'::regclass");
$subscriber->safe_psql('postgres', q{
SELECT srsubstate FROM pg_subscription_rel WHERE srrelid = 'tab_sync'::regclass;
}) eq 's' or die 'table sync did not complete before refresh';

$publisher->safe_psql('postgres', 'ALTER PUBLICATION pub_sync DROP TABLE tab_sync');
$subscriber->safe_psql('postgres', q{
SELECT injection_points_attach('subscription-refresh-before-origin-check', 'wait');
});
my $refresh = $subscriber->background_psql('postgres');
$refresh->query_until(qr/starting_refresh/, q{
\echo starting_refresh
ALTER SUBSCRIPTION sub_sync REFRESH PUBLICATION;
});
$subscriber->wait_for_event('client backend',
'subscription-refresh-before-origin-check');

$second_blocker->query_safe('COMMIT');
$publisher->safe_psql('postgres', 'INSERT INTO tab_first VALUES (2)');
wait_for_lock('logical replication apply worker', 'object',
"locks.pid = $apply_pid AND $subscription_lock");

$subscriber->safe_psql('postgres', q{
SELECT injection_points_wakeup('subscription-refresh-before-origin-check');
});
$refresh->quit or die 'refresh failed';
$publisher->safe_psql('postgres', 'INSERT INTO tab_first VALUES (3)');
$subscriber->poll_query_until('postgres', q{
SELECT (SELECT count(*) = 3 FROM tab_first) OR NOT subenabled
FROM pg_subscription WHERE subname = 'sub_sync';
}) or die 'apply neither caught up nor disabled the subscription';
is($subscriber->safe_psql('postgres', q{
SELECT subenabled FROM pg_subscription WHERE subname = 'sub_sync';
}), 't', 'refresh does not disable the subscription');

$subscriber->safe_psql('postgres', q{
SELECT injection_points_detach('subscription-refresh-before-origin-check');
DROP SUBSCRIPTION sub_sync;
DROP TABLE tab_first, tab_second, tab_sync;
});
$publisher->safe_psql('postgres', q{
DROP PUBLICATION pub_sync;
DROP TABLE tab_first, tab_second, tab_sync;
});
$sync_blocker->quit;
$first_blocker->quit;
$second_blocker->quit;

$subscriber->stop('fast');
$publisher->stop('fast');
done_testing();
