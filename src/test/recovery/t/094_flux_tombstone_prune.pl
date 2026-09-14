
# Copyright (c) 2026, PostgreSQL Global Development Group

# VACUUM prunes the delete-marked index tombstones a FLUX in-place key UPDATE
# leaves behind for a row that is still live.
#
# An UPDATE of an indexed column keeps the row at its TID, inserts the new
# (key, TID) entry and delete-marks the old one so older snapshots still reach
# the row by its old key.  The TID stays live, so the ordinary dead-TID test
# never lets VACUUM remove the tombstone.  It must go once no snapshot can need
# the old key, and not before.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_tomb');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

if (   !$node->check_extension('pageinspect')
	|| !$node->check_extension('amcheck')
	|| !$node->check_extension('pg_walinspect'))
{
	plan skip_all => 'pageinspect, amcheck or pg_walinspect not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE TABLE t (id int PRIMARY KEY, k int, v text) USING flux;
CREATE INDEX t_k ON t (k);
INSERT INTO t SELECT g, g, 'x' FROM generate_series(1, 10) g;
});

# The index fits on its root leaf page.
my $entries = q{SELECT live_items FROM bt_page_stats('t_k', 1)};
my $q = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
  . 'SELECT count(*) FROM t WHERE k = ';
my $by_key = sub {
	my ($s, $k) = @_;
	return $s->isa('PostgreSQL::Test::Cluster')
	  ? $s->safe_psql('postgres', $q . $k)
	  : $s->query_safe($q . $k);
};

# A snapshot taken before the key UPDATEs.
my $old = $node->background_psql('postgres');
$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
$old->query_safe('SELECT count(*) FROM t');

$node->safe_psql('postgres',
	join('', map { "UPDATE t SET k = k + 1000 WHERE id = 1;\n" } 1 .. 100));
is($node->safe_psql('postgres', $entries),
	'110', 'one tombstone per key UPDATE');

$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries),
	'110', 'tombstones kept while an older snapshot is open');
is($by_key->($old, 1),
	'1', 'older snapshot still finds the row by its old key');
is($by_key->($old, 100001), '0', 'older snapshot does not see the new key');
$old->query_safe('COMMIT');
$old->quit;

$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries),
	'10', 'VACUUM pruned the tombstones');
is($node->safe_psql('postgres', q{SELECT bt_index_check('t_k', true)}),
	'', 'index is consistent');
is($by_key->($node, 100001), '1', 'row found by its current key');
is($by_key->($node, 1001), '0', 'row not found by a superseded key');

# Back to a key whose tombstone was pruned: a fresh entry, ordering intact.
$node->safe_psql('postgres', 'UPDATE t SET k = 1 WHERE id = 1');
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries), '10', 'return to a pruned key');
is($by_key->($node, 1), '1', 'row found by the key it returned to');
is($node->safe_psql('postgres', q{SELECT bt_index_check('t_k', true)}),
	'', 'index is consistent after the return');

# Expression and partial indexes keep their tombstones (not evaluated under
# the index page lock).
$node->safe_psql(
	'postgres', q{
CREATE INDEX t_e ON t ((k + 0));
CREATE INDEX t_p ON t (k) WHERE id > 0;
UPDATE t SET k = 2000 WHERE id = 2;
VACUUM t;
});
is( $node->safe_psql(
		'postgres', q{SELECT string_agg(live_items::text, ',' ORDER BY i)
		 FROM (VALUES (1, 't_e'), (2, 't_p'), (3, 't_k')) v(i, n),
		 LATERAL bt_page_stats(n, 1)}),
	'11,11,10',
	'expression and partial index tombstones kept');

# An in-progress key UPDATE keeps its tombstone, and its rollback still
# restores the index.
my $w = $node->background_psql('postgres');
$w->query_safe('BEGIN');
$w->query_safe('UPDATE t SET k = -1 WHERE id = 1');
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries),
	'11', 'tombstone of an in-progress UPDATE kept');
$w->query_safe('ROLLBACK');
$w->quit;
is($by_key->($node, 1), '1', 'rolled-back key UPDATE: old key found');
is($by_key->($node, -1), '0', 'rolled-back key UPDATE: new key gone');
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries),
	'10', 'index back to one entry per row');
is($node->safe_psql('postgres', q{SELECT bt_index_check('t_k', true)}),
	'', 'index is consistent after the rollback');

# The newest version's writer can hold an older XID than an earlier writer.
# A snapshot taken between them reads the earlier version by its key; the
# tombstone stays while it is open, and on a standby removing it cancels it.
$node->backup('b');
my $standby = PostgreSQL::Test::Cluster->new('flux_tomb_standby');
$standby->init_from_backup($node, 'b', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"max_standby_streaming_delay = 0\nhot_standby_feedback = off");
$standby->start;

$node->safe_psql('postgres', 'CREATE TABLE other (i int)');
my $a = $node->background_psql('postgres');
$a->query_safe('BEGIN');
$a->query_safe('INSERT INTO other VALUES (1)');    # A takes its XID first
my $axid = $a->query_safe('SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'UPDATE t SET k = 7000 WHERE id = 3');    # B
$node->wait_for_replay_catchup($standby);

my $p = $node->background_psql('postgres');
my $s = $standby->background_psql('postgres', on_error_stop => 0);
foreach my $c ($p, $s)
{
	$c->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($by_key->($c, 7000), '1',
		'snapshot between the writers sees B\'s key');
}
$a->query_safe('UPDATE t SET k = 8000 WHERE id = 3');
$a->query_safe('COMMIT');
$a->quit;

$node->safe_psql('postgres', 'VACUUM t');
is($by_key->($p, 7000), '1', 'tombstone kept for the primary snapshot');
$p->query_safe('COMMIT');
$p->quit;

my $lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries), '10', 'tombstones pruned');
# VACUUM does not flush its WAL; a commit does.
$node->safe_psql('postgres', 'INSERT INTO other VALUES (2)');
is( $node->safe_psql(
		'postgres', qq{SELECT string_agg(substring(description
			from 'snapshotConflictHorizon: (\\d+)'), ',')
		FROM pg_get_wal_records_info('$lsn', pg_current_wal_lsn())
		WHERE resource_manager = 'Btree' AND record_type = 'DELETE'}),
	$axid,
	'removal conflicts with snapshots not seeing the older XID');
ok( $standby->wait_for_log(
		qr/terminating connection due to conflict with recovery/),
	'standby snapshot needing the tombstone was cancelled');
$s->{run}->kill_kill;
is($by_key->($standby, 8000), '1',
	'standby finds the row by its current key');

# A key UPDATE committed through a released savepoint, and one replayed on
# the standby, leave page images with a stale UNCOMMITTED hint (it is cleared
# by the committing backend, by top-level XID, and never by redo).  CLOG, not
# that hint, decides.
$node->safe_psql(
	'postgres', q{
BEGIN; SAVEPOINT s; UPDATE t SET k = 9000 WHERE id = 4; RELEASE s; COMMIT;
UPDATE t SET k = 9001 WHERE id = 5;
});
$node->wait_for_replay_catchup($standby);
$node->safe_psql('postgres', 'VACUUM t');
is($node->safe_psql('postgres', $entries),
	'10', 'tombstone of a committed subtransaction pruned');
$standby->promote;
$standby->safe_psql('postgres', 'VACUUM t');
is($standby->safe_psql('postgres', $entries),
	'10', 'promoted standby prunes tombstones of replayed UPDATEs');
is($standby->safe_psql('postgres', q{SELECT bt_index_check('t_k', true)}),
	'', 'promoted standby: index is consistent');

$standby->stop;
$node->stop;
done_testing();
