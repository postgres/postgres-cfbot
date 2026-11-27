# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that pg_rewind with a cleanly stopped standby as local source makes the
# target replay to the minRecoveryPoint of the source.
#
# Node A is the primary, B a standby that is used as source, and C a promoted
# standby that is rewound.
#
# This test does not make use of RewindTest as it requires three nodes.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use File::Copy;

my $tmp_folder = PostgreSQL::Test::Utils::tempdir;

# Return the minRecoveryPoint stored in the control file of a node.
sub min_recovery_point
{
	my $node = shift;
	my ($stdout, $stderr) =
	  run_command([ 'pg_controldata', $node->data_dir ]);
	my ($lsn) = $stdout =~ /^Minimum recovery ending location:\s+(\S+)$/m;
	return $lsn;
}

my $node_a = PostgreSQL::Test::Cluster->new('node_a');
$node_a->init(allows_streaming => 1);
$node_a->append_conf(
	'postgresql.conf', qq(
wal_keep_size = '100MB'
));
$node_a->start;

# Both tables get a row, so that their pages exist before the nodes diverge.
$node_a->safe_psql('postgres',
	'CREATE TABLE t1 AS SELECT 1 AS i; CREATE TABLE t2 AS SELECT 1 AS i;');

my $backup_name = 'my_backup';
$node_a->backup($backup_name);

my $node_b = PostgreSQL::Test::Cluster->new('node_b');
$node_b->init_from_backup($node_a, $backup_name, has_streaming => 1);
$node_b->start;

my $node_c = PostgreSQL::Test::Cluster->new('node_c');
$node_c->init_from_backup($node_a, $backup_name, has_streaming => 1);
$node_c->start;

# Promote C and insert into t1 on it.  pg_rewind copies only pages that the
# target has changed, so it will copy the page of t1 from B, but not the page
# of t2.
$node_a->wait_for_replay_catchup($node_c);
$node_c->promote;
$node_c->safe_psql('postgres', 'INSERT INTO t1 VALUES (2)');
$node_c->stop;

# On A, run a checkpoint followed by a transaction that changes both tables.
# B replays both, so the restartpoint of its shutdown is older than the
# transaction, while its minRecoveryPoint covers it.
$node_a->safe_psql('postgres',
	'CHECKPOINT; BEGIN; INSERT INTO t1 VALUES (3); INSERT INTO t2 VALUES (3); COMMIT;'
);
$node_a->wait_for_replay_catchup($node_b);
$node_b->stop;

# Keep a temporary postgresql.conf or it would be overwritten during the rewind.
my $node_c_pgdata = $node_c->data_dir;
copy("$node_c_pgdata/postgresql.conf", "$tmp_folder/postgresql.conf.tmp");

command_ok(
	[
		'pg_rewind',
		'--source-pgdata' => $node_b->data_dir,
		'--target-pgdata' => $node_c_pgdata,
		'--no-sync',
	],
	'pg_rewind with a stopped standby as local source');

move("$tmp_folder/postgresql.conf.tmp", "$node_c_pgdata/postgresql.conf");

is( min_recovery_point($node_c),
	min_recovery_point($node_b),
	'target replays to the minRecoveryPoint of the source standby');

# C now has the page of t1 with the row of that transaction, but the old page
# of t2.  End recovery as soon as C is consistent.  If that is before the
# transaction, the row never reaches t2, while the row in t1 shows up as soon
# as a new transaction on C gets the same xid.
$node_c->append_conf(
	'postgresql.conf', qq(
recovery_target = 'immediate'
recovery_target_action = 'promote'
));
$node_c->start;
$node_c->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "Timed out while waiting for promotion";
$node_c->safe_psql('postgres', 'SELECT pg_current_xact_id()');

is( $node_c->safe_psql(
		'postgres', q{
SELECT (SELECT string_agg(i::text, ',' ORDER BY i) FROM t1),
       (SELECT string_agg(i::text, ',' ORDER BY i) FROM t2)}),
	'1,3|1,3',
	'target shows the whole transaction replayed by the source standby');

done_testing();
