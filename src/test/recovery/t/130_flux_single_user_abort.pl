
# Copyright (c) 2026, PostgreSQL Global Development Group

# FLUX in single-user mode: a transaction updates rows in place, then
# replaces or drops storage the update touched, then rolls back.  There is
# no undo worker, so the backend itself must finish the rollback once the
# transaction's catalog changes are undone.  Then the same update applied
# again must give what it gives on heap, and amcheck must find the indexes
# intact.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($windows_os)
{
	plan skip_all => 'single-user mode is not tested on this platform';
}

my $node = PostgreSQL::Test::Cluster->new('flux_single_user_abort');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$node->stop;

foreach my $ddl (
	'DROP INDEX t_k',
	'REINDEX INDEX t_k',
	'ALTER TABLE t ALTER COLUMN k TYPE bigint',
	'TRUNCATE t', 'CLUSTER t USING t_k',
	'DROP TABLE t')
{
	foreach my $am ('heap', 'flux')
	{
		# With -j, a statement ends with a semicolon and an empty line.
		my $sql = <<"SQL";
DROP TABLE IF EXISTS t;

CREATE TABLE t (id int PRIMARY KEY, k int) USING $am;

CREATE INDEX t_k ON t (k);

INSERT INTO t SELECT g, g FROM generate_series(1, 200) g;

BEGIN;

UPDATE t SET k = k + 1000 WHERE id <= 50;

$ddl;

ROLLBACK;

UPDATE t SET k = k + 1000 WHERE id <= 50;

SELECT 'r:' || count(*) || '|' || sum(k) AS r FROM t;

SET enable_seqscan = off;

SELECT 'i:' || count(*) AS i FROM t WHERE k > 1000;

SELECT 'c:' || bt_index_check('t_k', true)::text || bt_index_check('t_pkey', true)::text AS c;

SQL
		my ($out, $err);
		my $ok = IPC::Run::run(
			[
				'postgres', '--single', '-F', '-j',
				'-c' => 'exit_on_error=true',
				'-D' => $node->data_dir,
				'postgres'
			],
			'<' => \$sql,
			'>' => \$out,
			'2>' => \$err);
		ok($ok, "$am, $ddl: single-user session succeeds")
		  or diag($err);
		my @got = $out =~ /"([ric]:[^"]*)"/g;
		is( join(' ', @got),
			'r:200|70100 i:50 c:',
			"$am, $ddl: rows, index scan and amcheck after the rollback");
		unlike($err, qr/WARNING/, "$am, $ddl: no warning");
	}
}

done_testing();
