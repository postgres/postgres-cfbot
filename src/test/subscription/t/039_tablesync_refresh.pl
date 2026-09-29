
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that ALTER SUBSCRIPTION ... REFRESH PUBLICATION can remove a table
# while the apply worker is about to mark it READY.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$subscriber->start;
$subscriber->safe_psql('postgres', 'CREATE EXTENSION injection_points');

# tab_other stays in the publication throughout; tab_sync is added and
# then removed again.
foreach my $node ($publisher, $subscriber)
{
	$node->safe_psql('postgres',
		'CREATE TABLE tab_other (a int); CREATE TABLE tab_sync (a int);');
}
$publisher->safe_psql('postgres',
	'CREATE PUBLICATION pub FOR TABLE tab_other');

# With copy_data = false, tab_other starts out READY, so only tab_sync goes
# through table synchronization below.  With disable_on_error, an error in
# the apply worker disables the whole subscription.
my $connstr = $publisher->connstr . ' dbname=postgres';
$subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub CONNECTION '$connstr' PUBLICATION pub WITH (copy_data = false, disable_on_error = true)"
);

# Make the apply worker wait just before it marks a table READY.
$subscriber->safe_psql('postgres',
	"SELECT injection_points_attach('tablesync-before-mark-ready', 'wait')");

# Add tab_sync and wait for its initial sync to finish (SYNCDONE).
$publisher->safe_psql('postgres', 'ALTER PUBLICATION pub ADD TABLE tab_sync');
$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION sub REFRESH PUBLICATION');
$subscriber->poll_query_until('postgres',
	"SELECT srsubstate = 's' FROM pg_subscription_rel WHERE srrelid = 'tab_sync'::regclass"
) or die "timed out waiting for tab_sync to reach SYNCDONE";

# Replicate a change, so the apply worker moves past the sync position and
# stops at the injection point.
$publisher->safe_psql('postgres', 'INSERT INTO tab_other VALUES (1)');
$subscriber->wait_for_event('logical replication apply worker',
	'tablesync-before-mark-ready');

# While the apply worker waits, remove tab_sync from the subscription.
$publisher->safe_psql('postgres',
	'ALTER PUBLICATION pub DROP TABLE tab_sync');
$subscriber->safe_psql('postgres',
	'ALTER SUBSCRIPTION sub REFRESH PUBLICATION');

# Let the apply worker continue.  It must skip the removed table rather
# than fail, since an error would disable the subscription.
$subscriber->safe_psql(
	'postgres', q{
SELECT injection_points_detach('tablesync-before-mark-ready');
SELECT injection_points_wakeup('tablesync-before-mark-ready');
});

# Replication of tab_other should carry on.
$publisher->safe_psql('postgres', 'INSERT INTO tab_other VALUES (2)');
$subscriber->poll_query_until('postgres',
	"SELECT (SELECT count(*) FROM tab_other) = 2 OR NOT subenabled FROM pg_subscription"
) or die "timed out waiting for the apply worker";
is( $subscriber->safe_psql(
		'postgres', 'SELECT subenabled FROM pg_subscription'),
	't',
	'subscription is still enabled');

done_testing();
