# Copyright (c) 2026, PostgreSQL Global Development Group
# Storage replacement must not alias savepoint-aborted tuple markers.
use strict;
use warnings FATAL => "all";
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
my $node = PostgreSQL::Test::Cluster->new("slog");
$node->init;
$node->append_conf("postgresql.conf", "autovacuum = off");
$node->start;

for my $inplace (0, 1)
{
	for my $ddl (
		"TRUNCATE t",
		"ALTER TABLE t ALTER COLUMN id TYPE bigint",
		"ALTER TABLE t SET ACCESS METHOD heap; ALTER TABLE t SET ACCESS METHOD AM"
	  )
	{
		my @result;
		for my $am ("heap", "flux")
		{
			my $change = $ddl;
			$change =~ s/AM/$am/g;
			my $create = "CREATE TABLE t(id int, w text) USING $am;";
			my $sql = ($inplace ? "BEGIN; $create" : "$create BEGIN;") . qq{
SAVEPOINT a;
INSERT INTO t SELECT -g, repeat('x',100) FROM generate_series(1,20) g;
ROLLBACK TO a;
RELEASE a;
SELECT set_config('v3h.oldfile', pg_relation_filenode('t')::text, true) IS NOT NULL;
$change;
INSERT INTO t SELECT g, repeat('x',100) FROM generate_series(1,20) g;
SELECT pg_relation_filenode('t') = current_setting('v3h.oldfile')::oid;
SELECT count(*), sum(id) FROM t;
SELECT count(*) FROM (SELECT * FROM t FOR UPDATE) s;
WITH s AS (UPDATE t SET w = 'updated' RETURNING *) SELECT count(*) FROM s;
WITH s AS (DELETE FROM t RETURNING *) SELECT count(*) FROM s;
COMMIT;
DROP TABLE t;
};
			push @result, $node->safe_psql("postgres", $sql);
		}
		my $samefile = $inplace && $ddl eq "TRUNCATE t" ? 't' : 'f';
		# SET AM heap on heap is a no-op, unlike flux -> heap -> flux.
		$result[0] =~ s/^t\n[tf]\n//;
		like($result[1], qr/^t\n$samefile\n/,
			"physical incarnation inplace=$inplace $ddl");
		$result[1] =~ s/^t\n[tf]\n//;
		is($result[0], "20|210\n20\n20\n20",
			"heap control inplace=$inplace $ddl");
		is($result[1], $result[0], "flux matches heap inplace=$inplace $ddl");
	}
}
$node->stop;
done_testing();
