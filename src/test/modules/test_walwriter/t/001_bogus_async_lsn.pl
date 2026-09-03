# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that XLogBackgroundFlush() honors the clamp applied by
# WaitXLogInsertionsToFinish() when the flush request is past the end
# of generated WAL.  Without the fix, the walwriter fails on one of
# XLogWrite()'s sanity checks (which one fires first depends on the
# WAL buffer state) and takes the server down, in both assert and
# production builds.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_writer_delay = 10ms
wal_writer_flush_after = 0
));
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_walwriter');

# Create a table for the later INSERT; this also guarantees that the
# segment switch performed by the injection is not a no-op.
$node->safe_psql('postgres', 'CREATE TABLE t AS SELECT 1 AS i');

my $log_offset = -s $node->logfile;

# Store a position where nothing has been inserted in asyncXactLSN.
my $injected = $node->safe_psql('postgres',
	'SELECT test_walwriter_bogus_async_lsn()');

# The walwriter's next cycle picks up the bogus request and logs the
# clamp.
$node->wait_for_log(qr/request to flush past end of generated WAL/,
	$log_offset);

# The advertised flush position must not include the bogus request.
my $result = $node->safe_psql('postgres',
	qq{SELECT pg_current_wal_flush_lsn() < '$injected'::pg_lsn});
is($result, 't', 'flush position stays below the bogus request');

# The walwriter must not have failed one of XLogWrite()'s sanity
# checks: no child process may have been terminated.
my $log = slurp_file($node->logfile, $log_offset);
unlike(
	$log,
	qr/terminating any other active server processes/,
	'no crash after the bogus flush request');

# Normal WAL activity gets past the bogus position.
$node->safe_psql('postgres', 'INSERT INTO t VALUES (2)');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$result = $node->safe_psql('postgres',
	qq{SELECT pg_current_wal_flush_lsn() > '$injected'::pg_lsn});
is($result, 't', 'flush position advances past the bogus request');

$node->stop;
done_testing();
