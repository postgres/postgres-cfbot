# Copyright (c) 2026, PostgreSQL Global Development Group

# Concurrent validation must count valid anchors, not just physical TIDs.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless ($ENV{enable_injection_points} // '') eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('concurrent_hot');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points; CREATE EXTENSION pageinspect');

# Hold an old snapshot during the updates to preserve every intermediate hop.
# The not-ready cases leave validation responsible for adding changed keys;
# ready indexes already have new entries, but unchanged indexes still do not.
for my $phase ('create', 'reindex_not_ready', 'reindex_ready')
{
	for my $case (
		['unchanged', '(id)', 'id', 'UPDATE t SET k = k + 1', "1|11\n2|21", 1],
		['changed', '(k)', 'k', 'UPDATE t SET k = k + 1', "1|11\n2|21", 1],
		['classic', '(id)', 'id', 'UPDATE t SET v = v + 1', "1|10\n2|20", 0],
		['suffix', '(id)', 'id', 'UPDATE t SET k = k + 1; UPDATE t SET v = v + 1; UPDATE t SET k = k + 1', "1|12\n2|22", 1],
		['aba', '(k)', 'k', 'UPDATE t SET k = k + 1; UPDATE t SET k = k - 1', "1|10\n2|20", 1],
		['expression', '((k + 1))', 'k + 1', 'UPDATE t SET k = k + 1', "1|11\n2|21", 0],
		['expression_unchanged', '((id + 1))', 'id + 1', 'UPDATE t SET k = k + 1', "1|11\n2|21", 1],
		['predicate', '(id) WHERE k > 0', 'id', 'UPDATE t SET k = -k; UPDATE t SET k = -k', "1|10\n2|20", 1],
		['include', '(id) INCLUDE (k)', 'id', 'UPDATE t SET k = k + 1', "1|11\n2|21", 1])
	{
		my ($name, $definition, $order, $updates, $expected, $hot_indexed) = @$case;
		my $label = "$phase $name";
		my $point = $phase eq 'reindex_ready' ? 'reindex-conc-index-built' : 'index-concurrently-built';
		my $index = "CREATE UNIQUE INDEX target ON t $definition";
		$node->safe_psql('postgres', q(
			CREATE TABLE t(id int, k int, v int) WITH (autovacuum_enabled=false);
			CREATE INDEX other_k ON t(k);
			CREATE INDEX other_id ON t(id);
			INSERT INTO t VALUES (1,10,0),(2,20,0);
		));
		$node->safe_psql('postgres', $index) unless $phase eq 'create';
		$node->safe_psql('postgres', "SELECT injection_points_attach('$point','wait')");
		my $builder = $node->background_psql('postgres');
		my $command = $phase eq 'create'
		  ? "CREATE UNIQUE INDEX CONCURRENTLY target ON t $definition"
		  : 'REINDEX INDEX CONCURRENTLY target';
		$builder->query_until(qr/launched/, "\\echo launched\n$command;\n");
		ok($node->poll_query_until('postgres', "SELECT count(*) = 1 FROM pg_stat_activity WHERE wait_event = '$point'"),
			"$label: build paused");
		my $old = $node->background_psql('postgres');
		$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM t');
		$node->safe_psql('postgres', $updates);
		is($old->query_safe('SELECT id,k FROM t ORDER BY id'), "1|10\n2|20",
			"$label: old transaction keeps original rows");
		is($node->safe_psql('postgres', q(
			SELECT bool_or((t_infomask2 & 2048) <> 0)
			FROM heap_page_items(get_raw_page('t', 0)) WHERE lp_flags = 1)),
			$hot_indexed ? 't' : 'f', "$label: expected hot-indexed path");
		$old->query_safe('COMMIT');
		$old->quit;
		$node->safe_psql('postgres', "SELECT injection_points_wakeup('$point')");
		$builder->query_safe('SELECT 1');
		$builder->quit;
		$node->safe_psql('postgres', "SELECT injection_points_detach('$point'); DROP INDEX other_k, other_id");
		my $query = "SELECT id,k FROM t WHERE k > 0 ORDER BY $order";
		my $settings = 'SET enable_seqscan=off; SET enable_bitmapscan=off; SET enable_indexonlyscan=off;';
		like($node->safe_psql('postgres', "$settings EXPLAIN (COSTS OFF) $query"),
			qr/Index Scan using target/, "$label: uses target index");
		is($node->safe_psql('postgres', "$settings $query"), $expected,
			"$label: index returns each row once in key order");
		is($node->safe_psql('postgres', q(
			SELECT indisready AND indisvalid FROM pg_index WHERE indexrelid='target'::regclass)),
			't', "$label: index is ready and valid");
		my ($stdout, $stderr);
		is($node->psql('postgres', 'INSERT INTO t SELECT * FROM t WHERE id = 1',
			stdout => \$stdout, stderr => \$stderr), 3, "$label: duplicate rejected");
		like($stderr, qr/duplicate key value violates unique constraint/, "$label: uniqueness enforced");
		$node->safe_psql('postgres', 'DROP TABLE t');
	}
}
$node->stop;
done_testing;
