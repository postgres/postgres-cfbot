
# Copyright (c) 2026, PostgreSQL Global Development Group

# What an UPDATE's SET clause proves about the indexes it cannot change.
# The executor logs each conclusion at DEBUG1; this reads the server log,
# so that other messages cannot disturb the test.  check_subattr checks
# that the conclusions are right.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('subattr');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Run one statement at log_min_messages = debug1, and return the
# conclusions it logged, as sorted "column/index" pairs.
sub concluded
{
	my ($sql) = @_;
	my $offset = -s $node->logfile;
	my @found;

	$node->safe_psql('postgres', "SET log_min_messages = debug1; $sql");
	my $log = slurp_file($node->logfile, $offset);
	while ($log =~ /UPDATE of column "(\w+)" cannot change index "(\w+)"/g)
	{
		push @found, "$1/$2";
	}
	return join(' ', sort @found);
}

$node->safe_psql(
	'postgres', q{
	CREATE TABLE subattr (id int PRIMARY KEY, d jsonb, a int[]);
	CREATE INDEX subattr_name ON subattr ((d->>'name'));
	CREATE INDEX subattr_tags ON subattr ((d->'tags'->>0));
	CREATE INDEX subattr_a2 ON subattr ((a[2]));
	INSERT INTO subattr VALUES (1, '{"name": "a", "tags": [1, 2]}', '{1,2,3}');
});

# nothing is concluded for an UPDATE of a single row
is(concluded(q{UPDATE subattr SET d = jsonb_set(d, '{age}', '1')}),
	'', 'single row: nothing concluded');

$node->safe_psql('postgres',
	q{INSERT INTO subattr VALUES (2, '{"name": "b", "tags": [1, 2]}', '{1,2,3}')}
);

is( concluded(q{UPDATE subattr SET d = jsonb_set(d, '{age}', '1')}),
	'd/subattr_name d/subattr_tags',
	'an unindexed key changes neither index');
is( concluded(q{UPDATE subattr SET d = jsonb_set(d, '{tags,1}', '9')}),
	'd/subattr_name', 'another array element: the name index only');
is( concluded(q{UPDATE subattr SET d = d - 'name', a[1] = 0}),
	'a/subattr_a2 d/subattr_tags',
	'deleting a key, and another array element');
is( concluded(q{UPDATE subattr SET d = d || '{"age": 2}'}),
	'd/subattr_name d/subattr_tags',
	'concatenating an object with other keys');

$node->safe_psql(
	'postgres', q{
	DROP TABLE subattr;
	CREATE TYPE subattr_pair AS (k int, v int);
	CREATE DOMAIN subattr_doc AS jsonb CHECK (VALUE ? 'name');
	CREATE TABLE subattr (id int PRIMARY KEY, d subattr_doc, p subattr_pair,
						  r jsonb, n jsonb) WITH (fillfactor = 50);
	CREATE INDEX subattr_name ON subattr ((d->>'name'));
	CREATE INDEX subattr_pk ON subattr (((p).k));
	CREATE INDEX subattr_whole ON subattr ((r->>'name')) WHERE r IS NOT NULL;
	CREATE INDEX subattr_row ON subattr (id) WHERE subattr IS NOT NULL;
	CREATE INDEX subattr_text ON subattr ((n::text));
	CREATE INDEX subattr_ep ON subattr ((jsonb_extract_path(n, 'a', 'b')));
	INSERT INTO subattr SELECT g, jsonb_build_object('name', 'n' || g),
		ROW(g, g)::subattr_pair, jsonb_build_object('name', 'r' || g),
		'{"a": {"b": 1}}'
	  FROM generate_series(1, 4) g;
});

is(concluded(q{UPDATE subattr SET d = jsonb_set(d, '{age}', '1')}),
	'd/subattr_name', 'a domain over jsonb');
is(concluded(q{UPDATE subattr SET p.v = 0}),
	'p/subattr_pk', 'another field of a composite');
is(concluded(q{UPDATE subattr SET p.k = (p).k + 10}),
	'', 'the indexed field of a composite');
is(concluded(q{UPDATE subattr SET r = jsonb_set(r, '{age}', '1')}),
	'', 'a column a predicate tests as a whole');
is(concluded(q{UPDATE subattr SET n = jsonb_set(n, '{c}', '1')}),
	'n/subattr_ep', 'jsonb_extract_path, another key');
is(concluded(q{UPDATE subattr SET n = jsonb_set(n, '{a,c}', '1')}),
	'n/subattr_ep', 'jsonb_extract_path, a sibling key');
is( concluded(
		q{UPDATE subattr SET d = jsonb_set(d, ARRAY[NULL, 'x'], '1') WHERE false}
	),
	'',
	'a null path element');
is( concluded(
		q{UPDATE subattr SET d = jsonb_set(d, '{age}', (SELECT d->'name'))}),
	'd/subattr_name', 'a sublink in the new value');
is( concluded(
		q{SET plan_cache_mode = force_generic_plan;
		  PREPARE subattr_p(text[], jsonb) AS
			UPDATE subattr SET d = jsonb_set(d, $1, $2);
		  EXECUTE subattr_p('{age}', '2');}),
	'd/subattr_name',
	'a generic plan, a parameter path');
is( concluded(
		q{SET plan_cache_mode = force_generic_plan;
		  PREPARE subattr_p(text[], jsonb) AS
			UPDATE subattr SET d = jsonb_set(d, $1, $2);
		  EXECUTE subattr_p('{name}', '"x"');}),
	'',
	'a generic plan, a parameter path to the indexed key');

$node->stop;
done_testing();
