
# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for amnotifydelete in logical replication apply: a delete or update
# applied by a subscription must tell the index the old value, as the same
# command run on the subscriber would.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# The apply worker's notices go only to the server log
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf', 'log_min_messages = notice');
$node_subscriber->start;

$node_publisher->safe_psql('postgres',
	"CREATE TABLE notify_rep (k int4 PRIMARY KEY, v int4, w int4)");
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION notify_pub FOR TABLE notify_rep");

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE EXTENSION dummy_index_am;
	CREATE TABLE notify_rep (k int4 PRIMARY KEY, v int4, w int4);
	CREATE INDEX notify_rep_idx ON notify_rep USING dummy_index_am (v);
	CREATE INDEX notify_rep_partial ON notify_rep USING dummy_index_am (v)
	  WHERE v > 100;
));
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION notify_sub CONNECTION '$publisher_connstr' PUBLICATION notify_pub"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'notify_sub');

# Run a command on the publisher, wait until it is applied, and return what
# the subscriber logged meanwhile
sub apply
{
	my ($sql) = @_;
	my $offset = -s $node_subscriber->logfile;

	$node_publisher->safe_psql('postgres', $sql);
	$node_publisher->wait_for_catchup('notify_sub');
	return slurp_file($node_subscriber->logfile, $offset);
}

my $log = apply(
	"INSERT INTO notify_rep VALUES (1, 10, 0), (2, 20, 0), (3, 150, 0)");
unlike($log, qr/notify_rep_(idx|partial): deleted/,
	'insert is not reported');

$log = apply("DELETE FROM notify_rep WHERE k = 1");
like($log, qr/notify_rep_idx: deleted 10/, 'delete reports the old value');
unlike($log, qr/notify_rep_partial: deleted/,
	'delete of a row outside a partial index is not reported to it');

$log = apply("UPDATE notify_rep SET v = 21 WHERE k = 2");
like($log, qr/notify_rep_idx: deleted 20/,
	'update of the indexed column reports the old value');

$log = apply("UPDATE notify_rep SET w = 1 WHERE k = 2");
unlike($log, qr/notify_rep_idx: deleted/,
	'HOT update is not reported');

$log = apply("DELETE FROM notify_rep WHERE k = 3");
like($log, qr/notify_rep_idx: deleted 150/,
	'delete of a row in a partial index reports to the full index');
like($log, qr/notify_rep_partial: deleted 150/,
	'delete of a row in a partial index reports to it');

$node_subscriber->stop;
$node_publisher->stop;

done_testing();
