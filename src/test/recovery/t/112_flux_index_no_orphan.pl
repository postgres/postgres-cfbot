# Copyright (c) 2026, PostgreSQL Global Development Group
# Inspect physical leaf entries, not just rows visible through an index scan.
# No VACUUM is run: undo must remove every newly inserted aborted key itself.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'injection points required'
  if $ENV{enable_injection_points} ne 'yes';
my $node = PostgreSQL::Test::Cluster->new('flux_no_orphan', port => 5890);
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
wal_consistency_checking = all
));
$node->start;
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE EXTENSION injection_points;
CREATE EXTENSION amcheck;
CREATE EXTENSION pg_walinspect;
CREATE EXTENSION pg_buffercache;
CREATE TABLE flush_marker(i int);
});
$node->backup('base');
my $standby =
  PostgreSQL::Test::Cluster->new('flux_no_orphan_standby', port => 5891);
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->start;

# Byte-order-independent integer keys, including on big-endian machines.
my ($a, $b, $c) = (16843009, 33686018, 50529027);
my $idx = 'SET enable_seqscan = off; SET enable_bitmapscan = off; '
  . 'SET enable_indexonlyscan = off; ';

sub make_table
{
	my ($t) = @_;
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t(k int, j int, marker int, padding text) USING flux;
CREATE INDEX ${t}_k ON $t(k);
INSERT INTO $t VALUES ($a, $a, 0, repeat('x', 500));
});
}

sub key_count
{
	my ($t, $hex) = @_;
	return "SELECT count(*) FROM bt_page_items('${t}_k', 1) "
	  . "WHERE data LIKE '$hex $hex $hex $hex%'";
}

sub check_clean
{
	my ($t, $what) = @_;
	$node->safe_psql('postgres', 'INSERT INTO flush_marker VALUES (0)');
	$node->wait_for_catchup($standby);
	foreach my $n ($node, $standby)
	{
		is($n->safe_psql('postgres', key_count($t, '02')),
			'0', "$what: no B entry on " . $n->name);
		is($n->safe_psql('postgres', key_count($t, '03')),
			'0', "$what: no C entry on " . $n->name);
		is( $n->safe_psql(
				'postgres', qq{
SELECT count(*) FROM bt_page_items('${t}_k', 1)
WHERE data LIKE '01 01 01 01%' AND itemlen = 16 AND NOT dead
}),
			'1',
			"$what: exactly one live A entry on " . $n->name);
		is( $n->safe_psql(
				'postgres', $idx . "SELECT count(*) FROM $t WHERE k = $a"),
			'1',
			"$what: A reaches row on " . $n->name);
	}
}

sub wait_reverted
{
	my ($t) = @_;
	# Raw page inspection cannot help revert the row itself.
	ok( $node->poll_query_until(
			'postgres',
			"SELECT position('\\x04040404'::bytea IN get_raw_page('$t', 0)) = 0"
		),
		"$t: worker restored the tuple");
}
make_table('inline_one');
$node->safe_psql('postgres', "BEGIN; UPDATE inline_one SET k = $b; ROLLBACK");
check_clean('inline_one', 'inline single update');

foreach my $path ([ 'aba', [ $b, $a ] ], [ 'abca', [ $b, $c, $a ] ])
{
	my ($t, $keys) = @$path;
	make_table($t);
	$node->safe_psql('postgres',
			'BEGIN; '
		  . join('; ', map { "UPDATE $t SET k = $_" } @$keys)
		  . '; ROLLBACK');
	check_clean($t, $t);
}
make_table('savepoint_one');
$node->safe_psql(
	'postgres', qq{
BEGIN; SAVEPOINT s; UPDATE savepoint_one SET k = $b;
ROLLBACK TO s; COMMIT;
});
check_clean('savepoint_one', 'savepoint');

$node->append_conf('postgresql.conf', 'undo_rollback_overflow_size = 0');
$node->restart;
make_table('worker_one');
$node->safe_psql(
	'postgres', qq{
SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');
BEGIN; UPDATE worker_one SET k = $b, marker = 67372036; ROLLBACK;
});
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
is($node->safe_psql('postgres', key_count('worker_one', '02')),
	'1', 'worker rollback has not run');
$node->safe_psql(
	'postgres', q{
SELECT injection_points_detach('pbu-undo-worker-before-apply');
SELECT injection_points_wakeup('pbu-undo-worker-before-apply');
});
wait_reverted('worker_one');
check_clean('worker_one', 'background worker');

make_table('crash_one');
my $bg = $node->background_psql('postgres');
$bg->query_safe("BEGIN; UPDATE crash_one SET k = $b, marker = 67372036");
$node->safe_psql('postgres', 'INSERT INTO flush_marker VALUES (1)');
$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;
wait_reverted('crash_one');
check_clean('crash_one', 'crash rollback');

$node->append_conf('postgresql.conf', 'undo_rollback_overflow_size = 1024');
$node->restart;
make_table('historic');
my $old = $node->background_psql('postgres');
$old->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM historic');
$node->safe_psql(
	'postgres', qq{
UPDATE historic SET k = $b;
BEGIN; UPDATE historic SET k = $a; ROLLBACK;
});
is($old->query_safe($idx . "SELECT count(*) FROM historic WHERE k = $a"),
	'1', 'older snapshot finds A after aborted revival');
is($node->safe_psql('postgres', key_count('historic', '01')),
	'1', 'rollback preserves the historic A tombstone');
$old->query_safe('COMMIT');
$old->quit;

# In the second UPDATE k revives A, but j inserts C for the first time.
make_table('mixed');
$node->safe_psql(
	'postgres', qq{
CREATE INDEX mixed_j ON mixed(j);
BEGIN;
UPDATE mixed SET k = $b, j = $b;
UPDATE mixed SET k = $a, j = $c;
ROLLBACK;
});
check_clean('mixed', 'mixed revived and inserted indexes');
is( $node->safe_psql(
		'postgres', q{SELECT count(*) FROM bt_page_items('mixed_j', 1)}),
	'1',
	'second changed index has only its original entry');

make_table('dropped');
$node->safe_psql(
	'postgres', qq{
BEGIN;
SAVEPOINT before_update;
UPDATE dropped SET k = $b;
SAVEPOINT before_drop;
DROP INDEX dropped_k;
ROLLBACK TO before_update;
COMMIT;
});
check_clean('dropped', 'index dropped inside savepoint');

# Crash after logging intent but before changing the index leaf.  A was
# delete-marked by a committed update; the incomplete revival must retain it.
make_table('ordered');
$node->safe_psql(
	'postgres', qq{
UPDATE ordered SET k = $b;
SELECT injection_points_attach('flux-index-after-amend', 'wait');
});
$bg = $node->background_psql('postgres');
$bg->query_until(qr/starting/,
	"BEGIN; SELECT 'starting'; UPDATE ordered SET k = $a, marker = 67372036;\n"
);
$node->wait_for_event('client backend', 'flux-index-after-amend');
$node->safe_psql('postgres', 'INSERT INTO flush_marker VALUES (2)');
$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;
wait_reverted('ordered');
$node->safe_psql('postgres', 'INSERT INTO flush_marker VALUES (0)');
$node->wait_for_catchup($standby);

foreach my $n ($node, $standby)
{
	is($n->safe_psql('postgres', key_count('ordered', '01')),
		'1', 'crash before revive retains A on ' . $n->name);
	is( $n->safe_psql(
			'postgres', $idx . "SELECT count(*) FROM ordered WHERE k = $b"),
		'1',
		'crash before revive restores B on ' . $n->name);
}
# A future undo page can reach disk before the crash while replay starts
# before the original insert.  Replaying that insert must not zero its list
# and then skip the amendment because the page LSN is already newer.
make_table('future_page');
$bg = $node->background_psql('postgres');
$bg->query_safe("UPDATE future_page SET k = $b");
$node->safe_psql('postgres', 'CHECKPOINT');
$bg->query_safe('UPDATE future_page SET marker = 1');
$bg->query_safe("BEGIN; UPDATE future_page SET k = $a, marker = 67372036");
$node->safe_psql('postgres', 'SELECT pg_buffercache_evict_all()');
$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;
wait_reverted('future_page');
is($node->safe_psql('postgres', key_count('future_page', '01')),
	'1', 'replay preserves the amended list on an already-flushed undo page');

# Every accepted near-page-size inline row must also support full-image undo.
# Discover the INSERT boundary rather than baking in tuple header sizes.
foreach my $nindexes (1, 20)
{
	my $t = "wide_$nindexes";
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE $t(k int, pad text STORAGE PLAIN) USING flux;
} . join('; ', map { "CREATE INDEX ${t}_k$_ ON $t(k)" } 1 .. $nindexes));
	my $limit;
	for my $pad (8000 .. 8192)
	{
		my ($out, $err) = ('', '');
		my $ret = $node->psql(
			'postgres',
			"INSERT INTO $t VALUES (1, repeat('x', $pad))",
			stdout => \$out,
			stderr => \$err);
		if ($ret != 0)
		{
			like(
				$err,
				qr/row is too big|tuple is too big|row size|tuple size/,
				"$nindexes indexes: INSERT reaches row size limit");
			$limit = $pad - 1;
			last;
		}
		$ret = $node->psql(
			'postgres', qq{
BEGIN;
UPDATE $t SET k = 2, pad = repeat('y', $pad);
ROLLBACK;
},
			stdout => \$out,
			stderr => \$err);
		is($ret, 0, "$nindexes indexes: pad $pad key update and rollback")
		  or diag $err;
		is( $node->safe_psql(
				'postgres',
				"SELECT k = 1 AND pad = repeat('x', $pad) FROM $t"),
			't',
			"$nindexes indexes: pad $pad before-image restored");
		$node->safe_psql(
			'postgres',
			join('; ',
				map { "SELECT bt_index_check('${t}_k$_', true)" }
				  1 .. $nindexes));
		$node->safe_psql('postgres', "TRUNCATE $t");
	}
	ok(defined $limit && $limit >= 8000,
		"$nindexes indexes: tested through the INSERT limit");
	$node->safe_psql('postgres',
		"INSERT INTO $t VALUES (1, repeat('x', $limit))");
	my $wide = $node->background_psql('postgres');
	$wide->query_safe(
		"BEGIN; UPDATE $t SET k = 2, pad = repeat('y', $limit)");
	my $wal_start =
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
	# Stay in one transaction so later records cannot include a previous
	# transaction header page.  Vary the starting byte with full before-images.
	for (1 .. 128)
	{
		$wide->query_safe(
			qq{
UPDATE $t SET k = 1, pad = repeat('x', $limit);
UPDATE $t SET k = 2, pad = repeat('y', $limit);
});
	}
	$wide->query_safe('ROLLBACK');
	$wide->quit;
	ok( $node->safe_psql(
			'postgres', qq{
SELECT EXISTS (
 SELECT start_lsn FROM pg_get_wal_block_info('$wal_start', pg_current_wal_lsn(), false)
 WHERE reldatabase = 9 AND resource_manager = 'FLUX'
 GROUP BY start_lsn HAVING count(*) = 3
)
}) eq 't',
		"$nindexes indexes: full before-image spans three undo pages");
	is( $node->safe_psql(
			'postgres', "SELECT k = 1 AND pad = repeat('x', $limit) FROM $t"),
		't',
		"$nindexes indexes: three-page rollback restores row");
	$node->safe_psql(
		'postgres',
		join('; ',
			map { "SELECT bt_index_check('${t}_k$_', true)" } 1 .. $nindexes)
	);
}

# A large reserved area and many record positions exercise page boundaries.
make_table('boundary');
$node->safe_psql('postgres',
	join('; ', map { "CREATE INDEX boundary_k$_ ON boundary(k)" } 1 .. 31));
my $start =
  $node->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$bg = $node->background_psql('postgres');
$bg->query_safe('BEGIN');
for (1 .. 256)
{
	$bg->query_safe("UPDATE boundary SET k = $b; UPDATE boundary SET k = $a");
}
$bg->query_safe('ROLLBACK');
$bg->quit;
$node->safe_psql('postgres', 'INSERT INTO flush_marker VALUES (3)');
ok( $node->safe_psql(
		'postgres', qq{
SELECT count(*) > 0 FROM pg_get_wal_records_info('$start', pg_current_wal_lsn())
WHERE record_type = 'AMEND_RECORD' AND description = 'blocks 2'
}) eq 't',
	'revive list amendment crosses an undo page boundary');
check_clean('boundary', 'cross-page amendment');
$node->safe_psql('postgres', q{SELECT bt_index_check('boundary_k', true)});

$standby->stop;
$node->stop;
done_testing();
