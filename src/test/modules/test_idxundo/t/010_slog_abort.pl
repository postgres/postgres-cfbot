# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new('slog_abort');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

for my $am ('heap', 'flux')
{
	$node->safe_psql('postgres',
		"CREATE TABLE t(id int PRIMARY KEY) USING $am");
	for my $body (
		'INSERT INTO t SELECT g FROM generate_series(1,20) g',
		'SAVEPOINT a; INSERT INTO t SELECT g FROM generate_series(1,20) g; ROLLBACK TO a',
		'INSERT INTO t VALUES (1) ON CONFLICT DO NOTHING',
		'SAVEPOINT a; INSERT INTO t VALUES (1) ON CONFLICT DO NOTHING; ROLLBACK TO a'
	  )
	{
		$node->safe_psql('postgres', "BEGIN; $body; ROLLBACK;" x 30);
		is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
			'0', "$am aborted rows invisible: $body");
	}
	$node->safe_psql('postgres', 'DROP TABLE t');
}
# Force the relation OID to differ from its physical file number.
$node->safe_psql(
	'postgres', q{
CREATE TABLE replay(id int) USING flux;
TRUNCATE replay;
CHECKPOINT;
INSERT INTO replay VALUES (1);
COPY replay FROM STDIN;
2
\.
});
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q{SELECT count(*) FROM replay}),
	'2', 'crash replay restored INSERT and COPY rows');
$node->stop;
done_testing();
