
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A crash between ROLLBACK PREPARED's abort record and the apply of its
# per-backend UNDO must still roll the transaction back.
#
# ROLLBACK PREPARED applies a prepared transaction's per-backend UNDO in a
# 2PC postabort callback, after the abort record.  Once that record is
# durable no prepared transaction is left to roll back again, so after a
# crash in between it is the per-backend UNDO recovery that must find the
# chain and roll it back.  FLUX updates in place, so a missed rollback leaves
# the new values on the page.  Readers would not see them (the writer did not
# commit), so the test also looks for them in the raw pages: they must be gone
# once the undo apply worker has run.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_rollback_prepared_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_prepared_transactions = 10
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('pageinspect'))
{
	plan skip_all => 'injection_points or pageinspect not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE TABLE t (id int PRIMARY KEY, v text) USING flux;
INSERT INTO t SELECT g, 'origorig' || g FROM generate_series(1, 500) g;
CHECKPOINT;
BEGIN;
-- Same length, so the update stays in place.
UPDATE t SET v = 'zQnewQz_' || id;
DELETE FROM t WHERE id > 450;
PREPARE TRANSACTION 'p1';
));

my $expected = $node->safe_psql('postgres',
	q(SELECT count(*), sum(length(v)) FROM t WHERE v LIKE 'origorig%'));
is($expected, '500|5392',
	'prepared changes are invisible before the rollback');

# Pages of t that still hold an updated value.
my $dirty_pages = q(
SELECT count(*) FROM generate_series(0, pg_relation_size('t') / 8192 - 1) b
WHERE position('zQnewQz_'::bytea IN get_raw_page('t', b::int)) > 0);
isnt($node->safe_psql('postgres', $dirty_pages),
	'0', 'the updated values are on the pages before the rollback');

# Hold ROLLBACK PREPARED just after its abort record, then crash.
$node->safe_psql('postgres',
	q(SELECT injection_points_attach('rollback-prepared-after-abort-record', 'wait'))
);
my $h = $node->background_psql('postgres', on_error_stop => 0);
$h->query_until(
	qr/start/, q(\echo start
ROLLBACK PREPARED 'p1';
));
$node->wait_for_event('client backend',
	'rollback-prepared-after-abort-record');
$node->stop('immediate');
$h->{run}->finish;

$node->start;
is($node->safe_psql('postgres', 'SELECT count(*) FROM pg_prepared_xacts'),
	'0', 'no prepared transaction after the crash');

sub check_rows
{
	my ($what) = @_;
	is( $node->safe_psql(
			'postgres',
			q(SELECT count(*), sum(length(v)) FROM t WHERE v LIKE 'origorig%')
		),
		$expected,
		"every row has its old value $what");
	is($node->safe_psql('postgres', q(SELECT count(*) FROM t)),
		'500', "no row is missing $what");
	is( $node->safe_psql(
			'postgres', q(
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM t WHERE id BETWEEN 1 AND 500 AND v = 'origorig' || id;)),
		'500',
		"index scans agree $what");
}
check_rows('after the restart');

# The undo apply worker reverts what recovery deferred.
ok($node->poll_query_until('postgres', "SELECT ($dirty_pages) = 0", 't'),
	'the rollback is applied to the pages');
check_rows('after the undo apply worker');

# And after another clean restart.
$node->restart;
check_rows('after a clean restart');

$node->stop;
done_testing();
