# Copyright (c) 2026, PostgreSQL Global Development Group

# A changed key in another index must not trigger expression evaluation during
# a unique check.  These cases deliberately do not build indexes concurrently.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('unique_hot');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

for my $case (
	['expression', '((id + 1))', '((id + 1))', 'k = 11', 1, 1],
	['include', '((id + 1)) INCLUDE (v)', '((id + 1))', 'k = 11', 1, 1],
	['partial', '((id + 1)) WHERE v >= 0', '((id + 1)) WHERE v >= 0', 'k = 11', 1, 1],
	['other_aba', '((id + 1))', '((id + 1))', 'k = 11; UPDATE t SET k = 10', 1, 1],
	['expression_changed', '((id + 1))', '((id + 1))', 'id = 2', 2, 0],
	['expression_equal', '((id % 2))', '((id % 2))', 'id = 3', 3, 0],
	['include_changed', '((id + 1)) INCLUDE (v)', '((id + 1))', 'v = 1', 1, 0],
	['predicate_aba', '((id + 1)) WHERE v >= 0', '((id + 1)) WHERE v >= 0', 'v = -1; UPDATE t SET v = 0', 1, 0],
	['null_not_distinct', '((NULLIF(id, 1))) NULLS NOT DISTINCT', '((NULLIF(id, 1)))', 'k = 11', 1, 1],
	['plain_changed', '(id)', '(id)', 'id = 2', 2, 1],
	['plain_aba', '(id)', '(id)', 'id = 2; UPDATE t SET id = 1', 1, 1])
{
	my ($name, $definition, $target, $update, $id, $hot_indexed) = @$case;
	$node->safe_psql('postgres', qq(
		CREATE TABLE t(id int, k int, v int);
		CREATE UNIQUE INDEX ti ON t $definition;
		CREATE INDEX tk ON t(k);
		INSERT INTO t VALUES (1,10,0);
	));
	# Keep every intermediate version, including the away-and-back hops.
	my $old = $node->background_psql('postgres');
	$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT * FROM t');
	$node->safe_psql('postgres', "UPDATE t SET $update");
	is($node->safe_psql('postgres', q(
		SELECT bool_or((t_infomask2 & 2048) <> 0)
		FROM heap_page_items(get_raw_page('t', 0)) WHERE lp_flags = 1)),
		$hot_indexed ? 't' : 'f', "$name: expected hot-indexed path");

	# Continue in the same backend after the error, not just a fresh connection
	# after crash recovery.  Verbose error output includes the SQLSTATE.
	my ($stdout, $stderr);
	is($node->psql('postgres', qq(
\\set ON_ERROR_STOP 0
\\set VERBOSITY verbose
SELECT pg_backend_pid() AS pid \\gset
INSERT INTO t VALUES ($id,20,0);
SELECT pg_backend_pid() = :pid;
), stdout => \$stdout, stderr => \$stderr),
		0, "$name: duplicate check does not terminate backend");
	like($stderr, qr/23505: duplicate key value violates unique constraint/,
		"$name: duplicate raises unique_violation");
	is($stdout, 't', "$name: same backend remains usable");

	is($node->safe_psql('postgres', qq(
		INSERT INTO t VALUES ($id,20,0) ON CONFLICT DO NOTHING RETURNING id)),
		'', "$name: ON CONFLICT DO NOTHING skips duplicate");
	my $k = $node->safe_psql('postgres', 'SELECT k + 1 FROM t');
	is($node->safe_psql('postgres', qq(
		INSERT INTO t VALUES ($id,20,0)
		ON CONFLICT $target DO UPDATE SET k = t.k + 1 RETURNING id,k)),
		"$id|$k", "$name: ON CONFLICT DO UPDATE updates existing row");
	is($node->safe_psql('postgres', 'SELECT count(*) FROM t'), '1',
		"$name: conflicts do not add rows");

	if ($name eq 'plain_changed' || $name eq 'expression_changed')
	{
		is($node->safe_psql('postgres',
			'INSERT INTO t VALUES (1,30,0) RETURNING id'), '1',
			"$name: old key is reusable");
	}
	is($old->query_safe('SELECT id,k,v FROM t'), '1|10|0',
		"$name: old snapshot retains original row");
	$old->query_safe('COMMIT');
	$old->quit;
	$node->safe_psql('postgres', 'DROP TABLE t');
}

# Ordinary unique indexes permit multiple NULL expression keys.
$node->safe_psql('postgres', q(
	CREATE TABLE t(id int, k int);
	CREATE UNIQUE INDEX ti ON t((NULLIF(id, 1)));
	CREATE INDEX tk ON t(k);
	INSERT INTO t VALUES (1,10);
	UPDATE t SET k = 11;
	INSERT INTO t VALUES (1,11) ON CONFLICT DO NOTHING;
));
is($node->safe_psql('postgres', 'SELECT count(*) FROM t'), '2',
	'NULLS DISTINCT permits a second NULL expression key');
$node->stop;
done_testing;
