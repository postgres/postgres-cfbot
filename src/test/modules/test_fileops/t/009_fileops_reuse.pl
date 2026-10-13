# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('reuse');
$node->init;
$node->append_conf('postgresql.conf',
	"wal_level = replica\nmax_prepared_transactions = 5\ncheckpoint_timeout = '1h'\n"
);
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops; CHECKPOINT');
$node->safe_psql(
	'postgres', q{
BEGIN;
SELECT test_fileops_create('base.dat', 384);
SELECT test_fileops_write('base.dat', 0, 'original');
COMMIT;
BEGIN;
SAVEPOINT a;
SELECT test_fileops_chmod('base.dat', 448);
SAVEPOINT b;
SELECT test_fileops_write('base.dat', 0, 'changed!');
RELEASE b;
ROLLBACK TO a;
COMMIT;
BEGIN;
SELECT test_fileops_write('base.dat', 0, 'prepared');
SELECT test_fileops_truncate('base.dat', 2);
PREPARE TRANSACTION 'bytes';
});
$node->stop('immediate');
$node->start;
$node->safe_psql('postgres', "ROLLBACK PREPARED 'bytes'");
my $dir = $node->data_dir;
for my $kind ('plain', 'sub', 'prepared')
{
	my $end =
	  $kind eq 'prepared' ? "PREPARE TRANSACTION 'p'; ROLLBACK PREPARED 'p';"
	  : $kind eq 'sub'    ? 'ROLLBACK TO s; COMMIT;'
	  :                     'ROLLBACK;';
	my $savepoint = $kind eq 'sub' ? 'SAVEPOINT s;' : '';
	$node->safe_psql('postgres',
		"BEGIN; $savepoint SELECT test_fileops_create('$kind.dat', 384); SELECT test_fileops_write('$kind.dat', 0, 'aborted'); $end"
	);
	ok( $node->poll_query_until(
			'postgres', "SELECT NOT test_fileops_file_exists('$kind.dat')"),
		"$kind: abort removed file");
	$node->safe_psql('postgres',
		"BEGIN; SELECT test_fileops_create('$kind.dat', 384); SELECT test_fileops_write('$kind.dat', 0, 'T2-committed'); COMMIT;"
	);
}
for my $restart (1 .. 2)
{
	$node->stop('immediate');
	$node->start;
	is(slurp_file("$dir/base.dat"),
		'original', "$restart: nested and prepared bytes restored");
	is((stat("$dir/base.dat"))[2] & 07777,
		0600, "$restart: nested mode restored");
	for my $kind ('plain', 'sub', 'prepared')
	{
		is(slurp_file("$dir/$kind.dat"),
			'T2-committed', "$restart $kind: committed reuse survives");
	}
}
$node->stop;
done_testing();
