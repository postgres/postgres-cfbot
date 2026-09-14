# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Rolling back a FLUX in-place UPDATE of an indexed column also reverses the
# index: the new (k_new, TID) entry is killed and the old (k_old, TID) entry
# un-delete-marked.  That reversal evaluates index expressions, which can
# fail.  A failure must leave the record to be retried, and the retry must
# redo the index reversal too.  If the tuple were restored first, the retry
# would find the record already undone and skip it, leaving the index broken
# for good.
#
# The index expression fails while ctl.fail is set.  The rollback goes to
# the undo apply worker (undo_rollback_overflow_size = 0), held at the
# injection point pbu-undo-worker-before-apply until ctl.fail is set; it
# then fails, and succeeds on a retry once ctl.fail is cleared.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_index_undo_fail');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('amcheck')
	|| !$node->check_extension('injection_points'))
{
	plan skip_all => 'amcheck or injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
CREATE EXTENSION injection_points;
CREATE TABLE ctl (fail bool);
INSERT INTO ctl VALUES (false);
CREATE FUNCTION kf(int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS
  $$ BEGIN
       IF $1 >= 100 AND (SELECT fail FROM public.ctl) THEN
         RAISE EXCEPTION 'kf fails';
       END IF;
       RETURN $1;
     END $$;
CREATE TABLE f (id int PRIMARY KEY, k int) USING flux;
CREATE INDEX f_kf ON f (kf(k));
INSERT INTO f SELECT g, g FROM generate_series(1, 4) g;
SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');
});

my $off = -s $node->logfile;
$node->safe_psql('postgres',
	'BEGIN; UPDATE f SET k = 100 WHERE id = 1; ROLLBACK;');
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
$node->safe_psql(
	'postgres', q{
UPDATE ctl SET fail = true;
SELECT injection_points_detach('pbu-undo-worker-before-apply');
SELECT injection_points_wakeup('pbu-undo-worker-before-apply');
});
ok( $node->wait_for_log(
		qr/undo apply worker.*ERROR: .*\n.*PL\/pgSQL function kf/, $off),
	'the index reversal fails in the undo apply worker');

$node->safe_psql('postgres', 'UPDATE ctl SET fail = false');
my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; ';
ok( $node->poll_query_until(
		'postgres', 'SELECT count(*) = 4 FROM f WHERE k < 100'),
	'the retry rolled the row back');
is( $node->safe_psql(
		'postgres', $idx . 'SELECT count(*) FROM f WHERE kf(k) = 1'),
	'1',
	'the k_old index entry is live again');
is( $node->safe_psql(
		'postgres', $idx . 'SELECT count(*) FROM f WHERE kf(k) = 100'),
	'0',
	'no k_new index entry');
$node->poll_query_until(
	'postgres',
	q{SELECT count(*) = 0 FROM pg_stat_activity
	  WHERE backend_type = 'undo apply worker'}
) or die 'undo apply worker did not finish';
is($node->safe_psql('postgres', q{SELECT bt_index_check('f_kf', true)}),
	'', 'index matches the table');
is( $node->safe_psql(
		'postgres',
		q{SELECT string_agg(id || ':' || k, ',' ORDER BY id) FROM f}),
	'1:1,2:2,3:3,4:4',
	'rows rolled back');
unlike(slurp_file($node->logfile, $off),
	qr/giving up/, 'the worker did not give up');

$node->stop;
done_testing();
