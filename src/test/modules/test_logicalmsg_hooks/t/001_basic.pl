# Copyright (c) 2026, PostgreSQL Global Development Group

# Test LogicalRepMessageHandle_hook.  The test module records every message it
# receives into public.test_logicalmsg_log on the subscriber, so most checks
# here are plain queries against that table.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $pub = PostgreSQL::Test::Cluster->new('publisher');
$pub->init(allows_streaming => 'logical');

# Keep the memory limit low so that the transactions below are streamed.
$pub->append_conf('postgresql.conf', "logical_decoding_work_mem = 64kB\n");
$pub->start;

my $sub = PostgreSQL::Test::Cluster->new('subscriber');
$sub->init(allows_streaming => 'logical');
$sub->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'test_logicalmsg_hooks'
max_parallel_apply_workers_per_subscription = 2
});
$sub->start;

$pub->safe_psql(
	'postgres', qq{
CREATE TABLE test (a int);
INSERT INTO test VALUES (1);
CREATE PUBLICATION pub FOR ALL TABLES;
});

my $pub_connstr = $pub->connstr . ' dbname=postgres';

$sub->safe_psql(
	'postgres', qq{
CREATE TABLE test (a int primary key);
CREATE SUBSCRIPTION sub CONNECTION '$pub_connstr' PUBLICATION pub WITH (messages = true, disable_on_error = true)
});

$sub->wait_for_subscription_sync($pub, 'sub');

# Number of messages recorded under the given prefix.
sub logged_count
{
	my ($prefix) = @_;

	return $sub->safe_psql('postgres',
		"SELECT count(*) FROM test_logicalmsg_log WHERE prefix = '$prefix'");
}

# Verify that a transactional message is passed to the hook, and that the
# message data given to the hook is correct.
$pub->safe_psql('postgres',
	q{SELECT pg_logical_emit_message(true, 'basic', 'transactional message')}
);
$pub->wait_for_catchup('sub');

is( $sub->safe_psql(
		'postgres',
		q{SELECT transactional, message_size, message FROM test_logicalmsg_log
		  WHERE prefix = 'basic'}),
	't|21|transactional message',
	"transactional message is passed to the hook");

# Verify that a non-transactional message is applied even if the transaction
# that emitted it rolls back.
$pub->safe_psql(
	'postgres', q{
BEGIN;
SELECT pg_logical_emit_message(false, 'nontransactional', 'emitted then rolled back');
ROLLBACK;
});

# Commit another message to flush WAL, as ROLLBACK doesn't flush.
$pub->safe_psql('postgres',
	q{SELECT pg_logical_emit_message(true, 'flush', 'flush')});
$pub->wait_for_catchup('sub');

is( $sub->safe_psql(
		'postgres',
		q{SELECT transactional, message FROM test_logicalmsg_log
		  WHERE prefix = 'nontransactional'}),
	'f|emitted then rolled back',
	"non-transactional message of an aborted transaction is applied");

# Verify that a transactional message emitted in a subtransaction that is
# rolled back is not passed to the hook.
$pub->safe_psql(
	'postgres', q{
BEGIN;
SAVEPOINT sp;
SELECT pg_logical_emit_message(true, 'subxact_aborted', 'not delivered');
ROLLBACK TO SAVEPOINT sp;
SELECT pg_logical_emit_message(true, 'subxact_committed', 'delivered');
COMMIT;
});
$pub->wait_for_catchup('sub');

is(logged_count('subxact_aborted'),
	0, "message of an aborted subtransaction is not applied");
is(logged_count('subxact_committed'),
	1, "message of a committed subtransaction is applied");

# Verify that transactional messages are applied in streaming transactions,
# both when the leader apply worker serializes the transaction to a file and
# when it hands the transaction to a parallel apply worker.
foreach my $mode ('on', 'parallel')
{
	$sub->safe_psql('postgres',
		"ALTER SUBSCRIPTION sub SET (streaming = $mode)");

	# Reconnect so that the new streaming mode takes effect immediately.
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION sub DISABLE');
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION sub ENABLE');

	$pub->safe_psql('postgres',
		q{SELECT pg_stat_reset_replication_slot('sub')});

	$pub->safe_psql(
		'postgres', qq{
BEGIN;
INSERT INTO test SELECT generate_series(1000, 6000);
SELECT pg_logical_emit_message(true, 'streaming_$mode', 'streamed message');
COMMIT;
});
	$pub->wait_for_catchup('sub');

	# Make sure the transaction really was streamed, so that this test cannot
	# silently degrade into a non-streaming one.
	$pub->poll_query_until('postgres',
		q{SELECT stream_txns > 0 FROM pg_stat_replication_slots WHERE slot_name = 'sub'}
	) or die "transaction was not streamed";

	is(logged_count("streaming_$mode"),
		1,
		"message in a streamed transaction is applied (streaming = $mode)");

	$pub->safe_psql('postgres', 'TRUNCATE test');
	$pub->wait_for_catchup('sub');
}

$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION sub SET (streaming = off)');

# Verify that ALTER SUBSCRIPTION ... SKIP skips a transactional message along
# with the rest of its transaction.  The message is emitted before the
# conflicting change, so the hook runs and records the message on the first
# attempt; that work must be rolled back with the failed transaction, and must
# not be redone once the transaction is skipped.
$pub->safe_psql('postgres', 'INSERT INTO test VALUES (1)');
$pub->wait_for_catchup('sub');

my $log_location = -s $sub->logfile;

$pub->safe_psql(
	'postgres', q{
BEGIN;
SELECT pg_logical_emit_message(true, 'skipped', 'rolled back, then skipped');
INSERT INTO test VALUES (1);
COMMIT;
});

# Wait until the conflict disables the subscription.
$sub->poll_query_until('postgres',
	q{SELECT subenabled = FALSE FROM pg_subscription WHERE subname = 'sub'});

# The hook did run before the transaction failed.
ok( $sub->log_contains(
		qr/LOG[^\n]+received message: [^\n]+prefix: skipped/,
		$log_location),
	"hook is called for a message in a transaction that later fails");

# ... but its work was rolled back with the transaction.
is(logged_count('skipped'), 0,
	"work done by the hook is rolled back with the remote transaction");

# Get the finish LSN of the failed transaction.
my $contents = slurp_file($sub->logfile, $log_location);
$contents =~
  qr/conflict detected on relation "public.test".*\n.*DETAIL:.*Could not apply remote change.*\n.*Key already exists in unique index "test_pkey", modified in transaction \d+: key .*, local row .*\n.*CONTEXT:.* for replication target relation "public.test" in transaction \d+, finished at ([[:xdigit:]]+\/[[:xdigit:]]+)/m
  or die "could not get error-LSN";
my $lsn = $1;

$log_location = -s $sub->logfile;

# Set skip LSN and re-enable the subscription.
$sub->safe_psql('postgres', qq{ALTER SUBSCRIPTION sub SKIP (lsn = '$lsn')});
$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION sub ENABLE');

# Wait for the failed transaction to be skipped.
$sub->poll_query_until('postgres',
	q{SELECT subskiplsn = '0/0' FROM pg_subscription WHERE subname = 'sub'});

ok( !$sub->log_contains(
		qr/LOG[^\n]+received message: [^\n]+prefix: skipped/,
		$log_location),
	"hook is not called for a message in a skipped transaction");
is(logged_count('skipped'), 0,
	"message in a skipped transaction is not applied");

$sub->stop;
$pub->stop;
done_testing();
