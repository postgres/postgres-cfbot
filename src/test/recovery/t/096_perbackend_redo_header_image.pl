# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A transaction's first undo record links the previous transaction's header
# to it (uur_next).  When that is the first change to the header's undo page
# since the last checkpoint, the WAL record carries a full-page image of the
# page.  If a later discard then removes the whole segment file holding the
# page and the server crashes, redo of the link finds the page neither in
# shared buffers nor on disk: it has to come from the image.  Redo used to
# ignore undo page images, read a zeroed page, and fail an assertion.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_redo_header_image');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 4GB
));
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION amcheck;
CREATE TABLE t (id int PRIMARY KEY, v text) USING flux;
INSERT INTO t SELECT g, repeat('x', 100) FROM generate_series(1, 20000) g;
));

# Hold the discard horizon back, so that the first transaction's undo is
# still there when the second one links its header.
my $hold = $node->background_psql('postgres');
$hold->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;');

my $s = $node->background_psql('postgres');
$s->query_safe("UPDATE t SET v = 'a' WHERE id = 1");
my $logno = $s->query_safe(
	"SELECT logno FROM pg_stat_get_undo_logs()
	  WHERE pid = pg_backend_pid() AND persistence = 'permanent'");
$s->query_safe('CHECKPOINT');

# The link to the first transaction's header: the first change to its page
# since the checkpoint, hence a full-page image.  Check the replay of this
# record against the primary's pages too.
$s->query_safe("SET wal_consistency_checking = 'FLUX'");
$s->query_safe('DELETE FROM t WHERE id = 2');
$s->query_safe('RESET wal_consistency_checking');

# More than a segment of undo, so that discarding it all removes the segment
# file holding the header page.
for (my $lo = 2; $lo < 20000; $lo += 1000)
{
	$s->query_safe(
		"UPDATE t SET v = repeat('y', 100)
					 WHERE id > $lo AND id <= $lo + 1000");
}
$hold->query_safe('COMMIT');

ok( $node->poll_query_until(
		'postgres', qq(
SELECT ('x' || discard)::bit(64)::bigint & ((1::bigint << 40) - 1) >= 1048576
  FROM pg_stat_get_undo_logs() WHERE logno = $logno)),
	'the discard worker discards past the first segment');
my $seg0 = $node->data_dir . sprintf('/base/undo/%06X.0000000000', $logno);
ok(!-e $seg0, 'the segment file holding the header page is gone');

$node->stop('immediate');
$s->{run}->kill_kill;
$hold->{run}->kill_kill;

my $integrity = q(
SELECT count(*), count(*) FILTER (WHERE id = 1 AND v = 'a'),
       count(*) FILTER (WHERE id = 2),
       count(*) FILTER (WHERE id > 2 AND v = repeat('y', 100))
  FROM t);

ok($node->start(fail_ok => 1),
	'crash recovery restores the header page from its image');
my $log = slurp_file($node->logfile);
unlike($log, qr/TRAP:|PANIC:/, 'no assertion failure or PANIC in recovery');

foreach my $round (
	'after crash recovery',
	'after a second crash',
	'after a clean restart')
{
	if ($round eq 'after a second crash')
	{
		$node->stop('immediate');
		$node->start;
	}
	elsif ($round eq 'after a clean restart')
	{
		$node->restart;
	}
	is($node->safe_psql('postgres', $integrity),
		'19999|1|0|19998', "committed changes intact $round");
	is( $node->safe_psql('postgres', "SELECT bt_index_check('t_pkey', true)"),
		'',
		"index consistent with the table $round");
}

# The recovered undo log still takes undo and rolls it back.
$node->safe_psql(
	'postgres', q(
BEGIN;
UPDATE t SET v = 'z' WHERE id <= 100;
DELETE FROM t WHERE id BETWEEN 101 AND 200;
ROLLBACK;
));
ok($node->poll_query_until('postgres', $integrity, '19999|1|0|19998'),
	'a rollback after recovery restores every row');

$node->stop;
done_testing();
