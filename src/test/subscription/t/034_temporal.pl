
# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Logical replication tests for temporal tables
#
# A table can use a temporal PRIMARY KEY or UNIQUE index as its REPLICA IDENTITY.
# This is a GiST index (not B-tree) and its last element uses WITHOUT OVERLAPS.
# That element restricts other rows with overlaps semantics instead of equality,
# but it is always at least as restrictive as a normal non-null unique index.
# Therefore we can still apply logical decoding messages to the subscriber.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# setup

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

my ($result, $stdout, $stderr);

sub create_tables()
{
	# create tables on publisher

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_no_key (id int4range, valid_at daterange, a text)"
	);

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
	);

	$node_publisher->safe_psql('postgres',
		"CREATE TABLE temporal_unique (id int4range, valid_at daterange, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
	);

	# create tables on subscriber

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_no_key (id int4range, valid_at daterange, a text)"
	);

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
	);

	$node_subscriber->safe_psql('postgres',
		"CREATE TABLE temporal_unique (id int4range, valid_at daterange, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
	);
}

sub drop_everything()
{
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_no_key");
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_pk");
	$node_publisher->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_unique");
	$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_no_key");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_pk");
	$node_subscriber->safe_psql('postgres',
		"DROP TABLE IF EXISTS temporal_unique");
	$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");
}

# #################################
# Test with REPLICA IDENTITY DEFAULT:
# #################################

create_tables();

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key DEFAULT');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk DEFAULT');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique DEFAULT');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_no_key" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_no_key DEFAULT");
# No need to test again with FOR PORTION OF

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_no_key DEFAULT");
($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE FOR PORTION OF temporal_no_key DEFAULT");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key DEFAULT');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk DEFAULT');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_unique" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_unique DEFAULT");
# No need to test again with FOR PORTION OF

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_unique DEFAULT");
($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE FOR PORTION OF temporal_unique DEFAULT");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique DEFAULT');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY FULL:
# #################################

create_tables();

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY FULL");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY FULL");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY FULL");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY FULL");

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key FULL');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a}, 'synced temporal_pk FULL');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique FULL');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_no_key FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_no_key FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key FULL');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk FULL');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique FULL');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY USING INDEX
# #################################

# create tables on publisher

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY USING INDEX temporal_pk_pkey");

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_unique (id int4range NOT NULL, valid_at daterange NOT NULL, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY USING INDEX temporal_unique_id_valid_at_key"
);

# create tables on subscriber

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_pk (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY USING INDEX temporal_pk_pkey");

$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_unique (id int4range NOT NULL, valid_at daterange NOT NULL, a text, UNIQUE (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY USING INDEX temporal_unique_id_valid_at_key"
);

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk USING INDEX');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique USING INDEX');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_pk FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_pk FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk USING INDEX');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_unique FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01' SET a = 'c' WHERE id = '[2,3)'"
);

$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_unique FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2001-01-01)|b
[2,3)|[2001-01-01,2002-01-01)|c
[2,3)|[2003-01-01,2010-01-01)|b
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique USING INDEX');

# cleanup

drop_everything();


# #################################
# Test with REPLICA IDENTITY NOTHING
# #################################

create_tables();

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY NOTHING");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY NOTHING");

$node_publisher->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_no_key REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_pk REPLICA IDENTITY NOTHING");

$node_subscriber->safe_psql('postgres',
	"ALTER TABLE temporal_unique REPLICA IDENTITY NOTHING");

# sync initial data:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_no_key NOTHING');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is($result, qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_pk NOTHING');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result,
	qq{[1,2)|[2000-01-01,2010-01-01)|a},
	'synced temporal_unique NOTHING');

# replicate with no key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_no_key (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_no_key SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_no_key" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_no_key NOTHING");
# No need to test again with FOR PORTION OF

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_no_key NOTHING");
($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_no_key FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_no_key" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_no_key NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_no_key ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_no_key NOTHING');

# replicate with a primary key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_pk (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_pk SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_pk" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_pk NOTHING");
# No need to test again with FOR PORTION OF

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_pk WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_pk" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_pk NOTHING");
($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_pk FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_pk" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_pk NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_pk ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_pk NOTHING');

# replicate with a unique key:

$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_unique (id, valid_at, a)
   VALUES ('[2,3)', '[2000-01-01,2010-01-01)', 'a'),
          ('[3,4)', '[2000-01-01,2010-01-01)', 'a'),
          ('[4,5)', '[2000-01-01,2010-01-01)', 'a')");

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"UPDATE temporal_unique SET a = 'b' WHERE id = '[2,3)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot update table "temporal_unique" because it does not have a replica identity and publishes updates
HINT:  To enable updating the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't UPDATE temporal_unique NOTHING");
# No need to test again with FOR PORTION OF

($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique WHERE id = '[3,4)'");
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE temporal_unique NOTHING");
($result, $stdout, $stderr) = $node_publisher->psql('postgres',
	"DELETE FROM temporal_unique FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' WHERE id = '[2,3)'"
);
is( $stderr,
	qq(psql:<stdin>:1: ERROR:  cannot delete from table "temporal_unique" because it does not have a replica identity and publishes deletes
HINT:  To enable deleting from the table, set REPLICA IDENTITY using ALTER TABLE.),
	"can't DELETE FOR PORTION OF temporal_unique NOTHING");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT * FROM temporal_unique ORDER BY id, valid_at");
is( $result, qq{[1,2)|[2000-01-01,2010-01-01)|a
[2,3)|[2000-01-01,2010-01-01)|a
[3,4)|[2000-01-01,2010-01-01)|a
[4,5)|[2000-01-01,2010-01-01)|a}, 'replicated temporal_unique NOTHING');

# cleanup

drop_everything();

# #################################
# Test with a lossy REPLICA IDENTITY index (multirange key)
#
# A temporal key over a multirange column is backed by a lossy GiST index: two
# distinct multiranges that share a bounding range are indistinguishable in the
# index, so an index probe can return candidate tuples that are not exact
# matches. The apply worker must recheck each candidate against the actual row
# values, otherwise it could apply the change to the wrong row.
# #################################

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_mltrng (id int4multirange, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_mltrng (id int4multirange, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);

# Two different multiranges that share the bounding range [1,5), with the same
# valid_at. They do not conflict (their ids are not equal), but the GiST index
# cannot tell them apart without a recheck.
$node_publisher->safe_psql(
	'postgres',
	"INSERT INTO temporal_mltrng (id, valid_at, a)
   VALUES ('{[1,5)}', '[2000-01-01,2010-01-01)', 'a'),
          ('{[1,2),[3,5)}', '[2000-01-01,2010-01-01)', 'b')");

$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub1 FOR ALL TABLES");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub1 CONNECTION '$publisher_connstr' PUBLICATION pub1"
);
$node_subscriber->wait_for_subscription_sync;

$result = $node_subscriber->safe_psql('postgres',
	"SELECT id, a FROM temporal_mltrng ORDER BY a");
is( $result, qq{{[1,5)}|a
{[1,2),[3,5)}|b}, 'synced temporal_mltrng lossy identity');

# Update each row. Each apply probes the identity index and gets *both* rows as
# candidates; without a recheck the wrong row would be updated.
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_mltrng SET a = 'a2' WHERE id = '{[1,5)}'");
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_mltrng SET a = 'b2' WHERE id = '{[1,2),[3,5)}'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT id, a FROM temporal_mltrng ORDER BY a");
is( $result, qq{{[1,5)}|a2
{[1,2),[3,5)}|b2}, 'replicated temporal_mltrng UPDATE to correct rows');

# Same for DELETE: remove only one of the two bounding-range twins.
$node_publisher->safe_psql('postgres',
	"DELETE FROM temporal_mltrng WHERE id = '{[1,2),[3,5)}'");

$node_publisher->wait_for_catchup('sub1');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT id, a FROM temporal_mltrng ORDER BY a");
is($result, qq{{[1,5)}|a2},
	'replicated temporal_mltrng DELETE of correct row');

# cleanup

$node_publisher->safe_psql('postgres', "DROP TABLE temporal_mltrng");
$node_subscriber->safe_psql('postgres', "DROP TABLE temporal_mltrng");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub1");
$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub1");

# #################################
# Test column lists and row filters interacting with FOR PORTION OF leftovers
# #################################

# A FOR PORTION OF update/delete emits the truncated row as an UPDATE/DELETE and
# each temporal leftover as an INSERT.  The leftover INSERTs must honor the
# publication's column list and row filter like any other INSERT.

# --- Column list: leftovers replicate only the listed columns ---

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_collist (id int4range, valid_at daterange, a text, b text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_collist (id int4range, valid_at daterange, a text, b text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO temporal_collist VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a', 'secret')"
);

# The publication replicates only (id, valid_at, a); b is excluded.  The column
# list includes the replica identity columns, as required.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_collist FOR TABLE temporal_collist (id, valid_at, a)");
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub_collist CONNECTION '$publisher_connstr' PUBLICATION pub_collist"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub_collist');

$node_publisher->safe_psql('postgres',
	"UPDATE temporal_collist FOR PORTION OF valid_at FROM '2004-01-01' TO '2006-01-01' SET a = 'x' WHERE id = '[1,2)'"
);
$node_publisher->wait_for_catchup('sub_collist');

# b is never replicated, so it is NULL on the subscriber for the truncated row
# and both leftovers:
$result = $node_subscriber->safe_psql('postgres',
	"SELECT id, valid_at, a, b FROM temporal_collist ORDER BY valid_at");
is( $result, qq{[1,2)|[2000-01-01,2004-01-01)|a|
[1,2)|[2004-01-01,2006-01-01)|x|
[1,2)|[2006-01-01,2010-01-01)|a|},
	'column list applies to FOR PORTION OF leftovers');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub_collist");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub_collist");
$node_publisher->safe_psql('postgres', "DROP TABLE temporal_collist");
$node_subscriber->safe_psql('postgres', "DROP TABLE temporal_collist");

# --- Row filter: leftovers are subject to the publication row filter ---

$node_publisher->safe_psql('postgres',
	"CREATE TABLE temporal_rowfilter (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_subscriber->safe_psql('postgres',
	"CREATE TABLE temporal_rowfilter (id int4range, valid_at daterange, a text, PRIMARY KEY (id, valid_at WITHOUT OVERLAPS))"
);
$node_publisher->safe_psql('postgres',
	"INSERT INTO temporal_rowfilter VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a')"
);

# Only rows whose validity starts before 2005 are replicated.  valid_at is part
# of the replica identity, so it may appear in the row filter.
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION pub_rowfilter FOR TABLE temporal_rowfilter WHERE (lower(valid_at) < '2005-01-01')"
);
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION sub_rowfilter CONNECTION '$publisher_connstr' PUBLICATION pub_rowfilter"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'sub_rowfilter');

# FOR PORTION OF splits [2000,2010) into leftovers [2000,2004) and [2006,2010)
# around the truncated [2004,2006).  The [2006,2010) leftover starts in 2006, so
# it fails the row filter and is not replicated.
$node_publisher->safe_psql('postgres',
	"UPDATE temporal_rowfilter FOR PORTION OF valid_at FROM '2004-01-01' TO '2006-01-01' SET a = 'x' WHERE id = '[1,2)'"
);
$node_publisher->wait_for_catchup('sub_rowfilter');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT id, valid_at, a FROM temporal_rowfilter ORDER BY valid_at");
is( $result, qq{[1,2)|[2000-01-01,2004-01-01)|a
[1,2)|[2004-01-01,2006-01-01)|x},
	'row filter applies to FOR PORTION OF leftovers');

$node_subscriber->safe_psql('postgres', "DROP SUBSCRIPTION sub_rowfilter");
$node_publisher->safe_psql('postgres', "DROP PUBLICATION pub_rowfilter");
$node_publisher->safe_psql('postgres', "DROP TABLE temporal_rowfilter");
$node_subscriber->safe_psql('postgres', "DROP TABLE temporal_rowfilter");

done_testing();
