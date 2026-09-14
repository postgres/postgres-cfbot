
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX row locks: no fixed shared memory, no fixed capacity.
#
# FLUX keeps a row lock's owner and strength out of the tuple (the page has
# only an ownerless FLUX_TUPLE_LOCKED bit).  They used to live in a shared
# flat hash sized at startup: 1.3 GB at max_connections = 100 whether or not
# FLUX was ever used, and still too small for one transaction to lock 200k
# rows ("could not store row lock in sLog").  The locks now live in a DSA
# created on first use.  Heap is the control for every case.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_connections = 20
));
$node->start;

# Nothing is allocated for FLUX row locks until a FLUX row is locked.
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_shmem_allocations WHERE name ~* 'slog flat|tuple slog'"
	),
	'0',
	'no fixed shared memory for FLUX row locks');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_dsm_registry_allocations WHERE name ~ '^slog_tuple_lock'"
	),
	'0',
	'no FLUX row-lock DSA before any FLUX row lock');

my $n = 1000000;
foreach my $am ('heap', 'flux')
{
	$node->safe_psql(
		'postgres', qq(
		CREATE TABLE par_$am (id int PRIMARY KEY) USING $am;
		INSERT INTO par_$am SELECT generate_series(1, $n);
		CREATE TABLE chi_$am (pid int REFERENCES par_$am) USING $am;
	));
}

foreach my $am ('heap', 'flux')
{
	# One transaction locks a million rows and holds them.
	my $locker = $node->background_psql('postgres', on_error_stop => 0);
	$locker->query_safe('BEGIN');
	my $cnt = $locker->query(
		"SELECT count(*) FROM (SELECT 1 FROM par_$am FOR KEY SHARE) s");
	is($cnt, $n, "$am: one transaction locks $n rows FOR KEY SHARE");

	# A DELETE of a locked row waits for the locker.
	my ($ret, $out, $err) = $node->psql('postgres',
		"SET lock_timeout = '1s'; DELETE FROM par_$am WHERE id = "
		  . ($n - 3));
	like(
		$err,
		qr/lock timeout/,
		"$am: DELETE of a row locked FOR KEY SHARE waits");
	# A non-key UPDATE does not conflict with KEY SHARE.
	($ret, $out, $err) = $node->psql('postgres',
		"SET lock_timeout = '1s'; SELECT 1 FROM par_$am WHERE id = 7 FOR NO KEY UPDATE"
	);
	is($ret, 0,
		"$am: FOR NO KEY UPDATE of a KEY SHARE locked row does not wait");

	if ($am eq 'flux')
	{
		is( $node->safe_psql(
				'postgres',
				"SELECT count(*) > 0 FROM pg_dsm_registry_allocations WHERE name ~ '^slog_tuple_lock'"
			),
			't',
			'flux: the row-lock DSA exists once a FLUX row is locked');
	}

	$locker->query('COMMIT');
	$locker->quit;

	# After COMMIT no lock remains.
	($ret, $out, $err) = $node->psql('postgres',
		"SET lock_timeout = '1s'; DELETE FROM par_$am WHERE id = "
		  . ($n - 3));
	is($ret, 0, "$am: after COMMIT the row can be deleted");

	# A bulk FK insert locks every parent row; a concurrent parent DELETE
	# must wait, and no orphan may result.
	my $fk = $node->background_psql('postgres', on_error_stop => 0);
	$fk->query_safe('BEGIN');
	$fk->query("INSERT INTO chi_$am SELECT generate_series(1, 300000)");
	($ret, $out, $err) = $node->psql('postgres',
		"SET lock_timeout = '1s'; DELETE FROM par_$am WHERE id = 299997");
	like(
		$err,
		qr/lock timeout/,
		"$am: parent DELETE waits for a 300k-row FK insert");
	$fk->query('COMMIT');
	$fk->quit;
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM chi_$am c WHERE NOT EXISTS (SELECT 1 FROM par_$am p WHERE p.id = c.pid)"
		),
		'0',
		"$am: no orphaned child rows");
}

done_testing();
