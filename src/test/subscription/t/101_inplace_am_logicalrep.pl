# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Logical replication of INSERT/UPDATE/DELETE into and out of the FLUX
# in-place table AM, in both directions.  Exercises FLUX as a subscriber
# (the apply worker's UPDATE/DELETE/INSERT paths) and as a decoding source
# (the same-size in-place UPDATE path and the batch INSERT path both have to
# produce records the logical decoder can turn into a change).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

sub roundtrip
{
	my ($pub_am, $sub_am) = @_;

	my $pub = PostgreSQL::Test::Cluster->new("pub_${pub_am}_${sub_am}");
	$pub->init(allows_streaming => 'logical');
	$pub->start;
	my $sub = PostgreSQL::Test::Cluster->new("sub_${pub_am}_${sub_am}");
	$sub->init;
	$sub->start;

	# Same schema, possibly different AM, on each side.
	$pub->safe_psql('postgres',
		"CREATE TABLE t (id int PRIMARY KEY, v int) USING $pub_am");
	$sub->safe_psql('postgres',
		"CREATE TABLE t (id int PRIMARY KEY, v int) USING $sub_am");

	$pub->safe_psql('postgres',
		"INSERT INTO t SELECT g, g FROM generate_series(1, 20) g");
	$pub->safe_psql('postgres', "CREATE PUBLICATION p FOR TABLE t");

	my $connstr = $pub->connstr . ' dbname=postgres';
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p");
	$sub->wait_for_subscription_sync($pub, 's');

	# Initial COPY sync.
	is($sub->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t'),
		'20|210', "$pub_am -> $sub_am: initial sync");

	# UPDATE (the aliasing / CAS-decode / crit-section paths), DELETE, INSERT.
	$pub->safe_psql('postgres', 'UPDATE t SET v = v + 100 WHERE id <= 10');
	$pub->safe_psql('postgres', 'DELETE FROM t WHERE id > 15');
	$pub->safe_psql('postgres', 'INSERT INTO t VALUES (100, 999)');
	$pub->wait_for_catchup('s');

	is($sub->safe_psql('postgres', 'SELECT count(*), sum(v) FROM t'),
		'16|2119', "$pub_am -> $sub_am: UPDATE/DELETE/INSERT streamed");

	# Exact row check for the updated rows.
	is($sub->safe_psql('postgres', 'SELECT v FROM t WHERE id = 5'),
		'105', "$pub_am -> $sub_am: UPDATE value correct on subscriber");

	$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');
	$sub->stop;
	$pub->stop;
}

# Into the FLUX AM and out of it, both directions.
roundtrip('heap', 'flux');
roundtrip('flux', 'heap');

done_testing();
