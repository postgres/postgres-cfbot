# Copyright (c) 2026, PostgreSQL Global Development Group
#
# amcheck on a FLUX table whose nbtree indexes hold delete-marked entries.
#
# A FLUX UPDATE that changes an indexed key keeps the row's TID and
# delete-marks the old (k_old, TID) entry.  The tombstone is the only entry
# under which a snapshot that still sees k_old reaches the row, and next to it
# there may be a live entry with the same key for a different row.  Neither
# state is corruption:
#
# - bt_index_check(heapallindexed) run while the UPDATE is uncommitted sees
#   the k_old version, so it must find the tombstone's fingerprint;
# - bt_index_check(checkunique) must not count a tombstone as a live
#   duplicate of a new row that took the old key.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_amcheck_deletemark');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION amcheck;
CREATE TABLE t (id int, k int) USING flux;
INSERT INTO t SELECT g, g FROM generate_series(1, 100) g;
CREATE INDEX t_k ON t (k);
CREATE TABLE u (id int, k int) USING flux;
INSERT INTO u SELECT g, g FROM generate_series(1, 100) g;
CREATE UNIQUE INDEX u_k ON u (k);
});

# heapallindexed while an in-place key UPDATE is in progress.
my $upd = $node->background_psql('postgres');
$upd->query_safe('BEGIN');
$upd->query_safe('UPDATE t SET k = k + 1000 WHERE id = 50');

my ($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_check('t_k', true)");
is($rc . $stderr, '0', 'heapallindexed passes during an in-place key UPDATE');

$upd->query_safe('COMMIT');
$upd->quit;

($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_check('t_k', true)");
is($rc . $stderr, '0', 'heapallindexed passes after the UPDATE commits');

# checkunique: row id=7 moves off k=7, a new row takes k=7.  The index holds
# a tombstone (7, TID of id=7) next to the live (7, TID of the new row).
$node->safe_psql('postgres', 'UPDATE u SET k = 1007 WHERE id = 7');
$node->safe_psql('postgres', 'INSERT INTO u VALUES (1000, 7)');
is($node->safe_psql('postgres', 'SELECT count(*) FROM u WHERE k = 7'),
	'1', 'one live row has k = 7');

($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_check('u_k', true, true)");
is( $rc . $stderr,
	'0',
	'checkunique ignores a tombstone next to a live entry with the same key');
($rc, $stdout, $stderr) = $node->psql('postgres',
	"SELECT bt_index_parent_check('u_k', true, true, true)");
is($rc . $stderr,
	'0', 'bt_index_parent_check(rootdescend, checkunique) passes');

$node->stop;
done_testing();
