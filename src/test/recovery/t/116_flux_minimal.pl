# Copyright (c) 2026, PostgreSQL Global Development Group

# A new relation synchronized without WAL must survive loss of its undo.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $am (qw(heap flux))
{
	my $node = PostgreSQL::Test::Cluster->new($am);
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq[
wal_level = minimal
wal_skip_threshold = 0
]);
	$node->start;
	$node->safe_psql(
		'postgres', qq[
		BEGIN;
		CREATE TABLE upd (id int, id2 int) USING $am;
		INSERT INTO upd SELECT g, g FROM generate_series(1,10000) g;
		COPY upd FROM STDIN;
10001	10001
\\.
		UPDATE upd SET id2 = id2 + 1;
		DELETE FROM upd;
		COMMIT;
	]);
	$node->stop('immediate');
	$node->start;
	my ($ret, $stdout, $stderr) =
	  $node->psql('postgres', 'SELECT count(*) FROM upd');
	is($ret, 0, "$am recovered table is readable") or diag $stderr;
	is($stdout, '0', "$am committed delete survives recovery");
	$node->stop;
}
done_testing();
