# Copyright (c) 2026, PostgreSQL Global Development Group
#
# ROLLBACK PREPARED must reverse-apply the prepared transaction's whole
# per-backend UNDO chain, not just its first record, both live and after a
# crash/restart between PREPARE and ROLLBACK PREPARED.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_2pc');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_prepared_transactions = 10
));
$node->start;

# Each AM gets a table whose prepared transaction updates and deletes rows on
# several pages; every row must come back after ROLLBACK PREPARED.
sub setup_table
{
	my ($name, $am) = @_;
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE $name (id int, v text) USING $am;
INSERT INTO $name SELECT g, 'orig' || g FROM generate_series(1, 2000) g;
));
}

sub prepare_changes
{
	my ($name, $gid) = @_;
	$node->safe_psql(
		'postgres', qq(
BEGIN;
UPDATE $name SET v = 'upd' WHERE id IN (1, 500, 1000, 1500, 2000);
DELETE FROM $name WHERE id IN (10, 1200);
PREPARE TRANSACTION '$gid';
));
}

sub check_restored
{
	my ($name, $what) = @_;
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*), count(*) FILTER (WHERE v = 'orig' || id) FROM $name"
		),
		'2000|2000',
		"$what: every row restored");
}

foreach my $am ('flux')
{
	setup_table("live_$am", $am);
	prepare_changes("live_$am", "live_$am");
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'live_$am'");
	check_restored("live_$am", "$am ROLLBACK PREPARED");

	setup_table("crash_$am", $am);
	prepare_changes("crash_$am", "crash_$am");
}

# A prepared transaction with undo in both a permanent and an unlogged undo
# log must have both chains applied by ROLLBACK PREPARED.
foreach my $am ('flux')
{
	$node->safe_psql(
		'postgres', qq(
CREATE TABLE mp_$am (id int, v text) USING $am;
CREATE UNLOGGED TABLE mu_$am (id int, v text) USING $am;
INSERT INTO mp_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
INSERT INTO mu_$am SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
UPDATE mp_$am SET v = 'upd';
UPDATE mu_$am SET v = 'upd';
PREPARE TRANSACTION 'mixed_$am';
ROLLBACK PREPARED 'mixed_$am';
));
	is( $node->safe_psql(
			'postgres', qq(
SELECT (SELECT count(*) FROM mp_$am WHERE v = 'orig' || id) +
       (SELECT count(*) FROM mu_$am WHERE v = 'orig' || id))),
		'20',
		"$am ROLLBACK PREPARED of permanent and unlogged undo");
}

# Crash with both transactions still prepared, then resolve after restart.
$node->stop('immediate');
$node->start;

foreach my $am ('flux')
{
	$node->safe_psql('postgres', "ROLLBACK PREPARED 'crash_$am'");
	check_restored("crash_$am", "$am ROLLBACK PREPARED after crash");
}

$node->stop;
done_testing();
