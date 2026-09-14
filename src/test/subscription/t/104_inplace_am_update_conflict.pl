
# Copyright (c) 2026, PostgreSQL Global Development Group

# An UPDATE the apply worker cannot apply because its new key already
# exists must be reported as a conflict (update_exists, or
# multiple_unique_conflicts when it collides in several unique indexes), as
# for heap.  An in-place AM maintains the indexes of a key change inside
# table_tuple_update, where a duplicate raises a plain unique violation, so
# the apply worker has to look for the conflict before the update.  A key
# update that collides only with the row itself is no conflict.
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

	my $ddl =
	  "CREATE TABLE c (a int PRIMARY KEY, b int UNIQUE, c int UNIQUE) USING $am";
	$pub->safe_psql('postgres', $ddl);
	$sub->safe_psql('postgres', $ddl);
	$pub->safe_psql('postgres', 'CREATE PUBLICATION p FOR TABLE c');
	my $connstr = $pub->connstr . ' dbname=postgres';
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p WITH (disable_on_error = true)"
	);
	$sub->wait_for_subscription_sync($pub, 's');

	$pub->safe_psql('postgres', 'INSERT INTO c VALUES (5, 5, 5)');
	$pub->wait_for_catchup('s');

	# Wait for the apply worker to stop on the error, then return the log
	# written since $offset.
	my $error_log = sub {
		my ($offset) = @_;
		$sub->poll_query_until('postgres',
			"SELECT NOT subenabled FROM pg_subscription WHERE subname = 's'")
		  or die "timed out waiting for the subscription to be disabled";
		return slurp_file($sub->logfile, $offset);
	};

	# The new row collides with three local rows, one per unique index.
	$sub->safe_psql('postgres',
		'INSERT INTO c VALUES (6, 6, 6), (7, 7, 7), (8, 8, 8)');
	my $offset = -s $sub->logfile;
	$pub->safe_psql('postgres',
		'UPDATE c SET a = 6, b = 7, c = 8 WHERE a = 5');
	my $log = $error_log->($offset);
	like(
		$log,
		qr/conflict detected on relation "public.c": conflict=multiple_unique_conflicts.*
.*Could not apply remote change: remote row \(6, 7, 8\), replica identity \(a\)=\(5\).*
.*Key already exists in unique index "c_pkey", modified in transaction .*: key \(a\)=\(6\), local row \(6, 6, 6\).*
.*Key already exists in unique index "c_b_key", modified in transaction .*: key \(b\)=\(7\), local row \(7, 7, 7\).*
.*Key already exists in unique index "c_c_key", modified in transaction .*: key \(c\)=\(8\), local row \(8, 8, 8\)/,
		"$am: multiple_unique_conflicts detected during update");
	unlike(
		$log,
		qr/duplicate key value violates unique constraint/,
		"$am: no plain unique violation instead of the conflict");

	# Remove the local rows; the retried transaction applies.
	$sub->safe_psql('postgres', 'DELETE FROM c WHERE a IN (6, 7, 8)');
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s ENABLE');
	$pub->wait_for_catchup('s');
	is($sub->safe_psql('postgres', 'SELECT a, b, c FROM c'),
		'6|7|8', "$am: update applied once the conflicting rows are gone");

	# One unique index: update_exists.
	$sub->safe_psql('postgres', 'INSERT INTO c VALUES (9, 9, 9)');
	$offset = -s $sub->logfile;
	$pub->safe_psql('postgres', 'UPDATE c SET a = 9 WHERE a = 6');
	$log = $error_log->($offset);
	like(
		$log,
		qr/conflict detected on relation "public.c": conflict=update_exists.*
.*Could not apply remote change: remote row \(9, 7, 8\), replica identity \(a\)=\(6\).*
.*Key already exists in unique index "c_pkey", modified in transaction .*: key \(a\)=\(9\), local row \(9, 9, 9\)/,
		"$am: update_exists detected");
	$sub->safe_psql('postgres', 'DELETE FROM c WHERE a = 9');
	$sub->safe_psql('postgres', 'ALTER SUBSCRIPTION s ENABLE');
	$pub->wait_for_catchup('s');
	is($sub->safe_psql('postgres', 'SELECT a, b, c FROM c'),
		'9|7|8', "$am: update_exists update applied after the fix-up");

	# A key update whose other unique keys stay the same matches only the
	# row itself in those indexes: no conflict.
	$offset = -s $sub->logfile;
	$pub->safe_psql('postgres', 'UPDATE c SET b = 70 WHERE a = 9');
	$pub->safe_psql('postgres', 'UPDATE c SET a = 10 WHERE a = 9');
	$pub->wait_for_catchup('s');
	is($sub->safe_psql('postgres', 'SELECT a, b, c FROM c'),
		'10|70|8',
		"$am: key updates that collide only with the row itself apply");
	is( $sub->safe_psql(
			'postgres',
			"SELECT subenabled FROM pg_subscription WHERE subname = 's'"),
		't',
		"$am: no false conflict");
	unlike(
		slurp_file($sub->logfile, $offset),
		qr/conflict detected/,
		"$am: no conflict reported for a self-match");

	$sub->stop;
	$pub->stop;
}

done_testing();
