# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that a standby that has replayed up to the last_commit_lsn a session
# was shown shows that session's commit.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf',
	'max_prepared_transactions = 2');
$node_primary->start;
$node_primary->safe_psql(
	'postgres', q[
	CREATE TABLE commit_lsn_test (a int);
	CREATE PROCEDURE two_commits() LANGUAGE plpgsql AS $$
	BEGIN
		INSERT INTO commit_lsn_test VALUES (3);
		COMMIT;
		INSERT INTO commit_lsn_test VALUES (4);
		COMMIT;
	END $$;
]);
$node_primary->backup('backup');

# A standby that replays each commit a second late, so that the waits below
# have something to wait for.
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'backup', has_streaming => 1);
$node_standby->append_conf(
	'postgresql.conf', qq[
	max_prepared_transactions = 2
	recovery_min_apply_delay = '1s'
]);
$node_standby->start;

my $timeout = $PostgreSQL::Test::Utils::timeout_default;

foreach my $case (
	[ 'autocommit INSERT', 'INSERT INTO commit_lsn_test VALUES (1);', 1 ],
	[
		'explicit transaction',
		'BEGIN; INSERT INTO commit_lsn_test VALUES (2); COMMIT;', 2
	],
	[ 'procedure that commits twice', 'CALL two_commits();', 4 ],
	[
		'COMMIT PREPARED',
		"BEGIN; INSERT INTO commit_lsn_test VALUES (5);
		 PREPARE TRANSACTION 'p'; COMMIT PREPARED 'p';", 5
	])
{
	my ($desc, $sql, $value) = @$case;

	my $lsn =
	  $node_primary->safe_psql('postgres', "$sql SHOW last_commit_lsn;");
	my $output = $node_standby->safe_psql(
		'postgres', qq[
		WAIT FOR LSN '$lsn' WITH (timeout '${timeout}s');
		SELECT count(*) FROM commit_lsn_test WHERE a = $value;
	]);
	is($output, "success\n1", "$desc is visible on the standby at $lsn");
}

# Transactions that write no commit record leave the value alone.
my @lsns = split /\n/, $node_primary->safe_psql(
	'postgres', q[
	INSERT INTO commit_lsn_test VALUES (6);
	SHOW last_commit_lsn;
	BEGIN; INSERT INTO commit_lsn_test VALUES (7); ROLLBACK;
	SHOW last_commit_lsn;
	BEGIN; INSERT INTO commit_lsn_test VALUES (8); PREPARE TRANSACTION 'q';
	SHOW last_commit_lsn;
	ROLLBACK PREPARED 'q';
	SHOW last_commit_lsn;
]);
is_deeply(
	[ @lsns[ 1 .. 3 ] ],
	[ ($lsns[0]) x 3 ],
	'ROLLBACK, PREPARE TRANSACTION and ROLLBACK PREPARED leave it alone');

is( $node_primary->safe_psql(
		'postgres', q[
		BEGIN; INSERT INTO commit_lsn_test VALUES (9); ROLLBACK;
		SHOW last_commit_lsn;
	]),
	'0/00000000',
	'a session that has committed nothing shows 0/00000000');

done_testing();
