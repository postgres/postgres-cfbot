# Copyright (c) 2026, PostgreSQL Global Development Group

# Exercise sequential and indexed replica identity lookup with heap controls.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

for my $am ('heap', 'flux')
{
	my $pub = PostgreSQL::Test::Cluster->new("pub_$am");
	$pub->init(allows_streaming => 'logical');
	$pub->start;
	my $sub = PostgreSQL::Test::Cluster->new("sub_$am");
	$sub->init;
	$sub->start;

	my $ddl = qq{
		CREATE TABLE d (id int PRIMARY KEY, v text) USING $am;
		CREATE TABLE f (id int, v text) USING $am;
		ALTER TABLE f REPLICA IDENTITY FULL;
		CREATE TABLE i (id int NOT NULL, v text) USING $am;
		CREATE UNIQUE INDEX i_key ON i(id);
		ALTER TABLE i REPLICA IDENTITY USING INDEX i_key;
	};
	$pub->safe_psql('postgres', $ddl);
	$sub->safe_psql('postgres', $ddl);
	$pub->safe_psql('postgres', 'CREATE PUBLICATION p FOR TABLE d, f, i');
	my $connstr = $pub->connstr . ' dbname=postgres';
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p");
	$sub->wait_for_subscription_sync($pub, 's');

	for my $tab ('d', 'f', 'i')
	{
		$pub->safe_psql('postgres',
			"INSERT INTO $tab VALUES (1, 'one'), (2, 'two')");
		$pub->wait_for_catchup('s');
		$pub->safe_psql('postgres', "UPDATE $tab SET v = 'new' WHERE id = 1");
		$pub->wait_for_catchup('s');
		is( $sub->safe_psql('postgres', "SELECT id, v FROM $tab ORDER BY id"),
			"1|new\n2|two",
			"$am $tab update");
		$pub->safe_psql('postgres', "DELETE FROM $tab WHERE id = 2");
		$pub->wait_for_catchup('s');
		is( $sub->safe_psql('postgres', "SELECT id, v FROM $tab ORDER BY id"),
			'1|new',
			"$am $tab delete");
	}

	# A FULL old image must carry the toasted value needed by a row filter.
	for my $node ($pub, $sub)
	{
		$node->safe_psql(
			'postgres', qq{
			CREATE TABLE toast_filter (id int, b text, c text) USING $am;
			ALTER TABLE toast_filter ALTER COLUMN c SET STORAGE EXTERNAL;
			ALTER TABLE toast_filter REPLICA IDENTITY FULL;
		});
	}
	$pub->safe_psql(
		'postgres', q{
		ALTER PUBLICATION p ADD TABLE toast_filter WHERE (length(c) > 1000);
	});
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s REFRESH PUBLICATION');
	$sub->wait_for_subscription_sync($pub, 's');
	$pub->safe_psql(
		'postgres', q{
		INSERT INTO toast_filter VALUES (1, 'old', repeat('toast', 1000));
	});
	$pub->wait_for_catchup('s');
	$pub->safe_psql('postgres', "UPDATE toast_filter SET b = '1'");
	$pub->wait_for_catchup('s');
	is( $sub->safe_psql(
			'postgres', 'SELECT id, b, length(c) FROM toast_filter'),
		'1|1|5000',
		"$am toasted row filter update");
	$pub->safe_psql('postgres', 'DELETE FROM toast_filter');
	$pub->wait_for_catchup('s');
	is($sub->safe_psql('postgres', 'SELECT count(*) FROM toast_filter'),
		'0', "$am toasted row filter delete");
	$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');
	$sub->stop;
	$pub->stop;
}
done_testing();
