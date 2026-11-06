
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

#
# Test pg_rewind interaction with wal_log_hints:
# - Error out when wal_log_hints=off and no data checksums
# - Succeeds after reload to on
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary without data checksums and with wal_log_hints=off
my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1, no_data_checksums => 1);
$node_primary->append_conf(
	'postgresql.conf', qq{
wal_log_hints = off
wal_level = replica
wal_keep_size = 64MB
autovacuum = off
});
$node_primary->start;

$node_primary->safe_psql('postgres',
	"CREATE TABLE t(id int); INSERT INTO t SELECT generate_series(1,100)");
$node_primary->safe_psql('postgres', "CHECKPOINT");

# Create standby, diverge
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_primary->backup('my_backup');
$node_standby->init_from_backup($node_primary, 'my_backup',
	has_streaming => 1);
$node_standby->start;
$node_primary->wait_for_catchup($node_standby);

$node_standby->promote;
$node_standby->safe_psql('postgres', "INSERT INTO t VALUES (999)");
$node_primary->safe_psql('postgres', "INSERT INTO t VALUES (888)");
$node_primary->safe_psql('postgres', "CHECKPOINT");
$node_primary->stop;

# pg_rewind must refuse: wal_log_hints=off
command_fails_like(
	[
		'pg_rewind',
		'--target-pgdata' => $node_primary->data_dir,
		'--source-server' => $node_standby->connstr('postgres'),
		'--no-sync',
	],
	qr/target server needs to use either data checksums or "wal_log_hints = on"/,
	'pg_rewind refuses with wal_log_hints=off and no data checksums');

# Restart primary and enable wal_log_hints via reload
$node_primary->start;
$node_primary->append_conf('postgresql.conf', 'wal_log_hints = on');
$node_primary->reload;

my $result = $node_primary->safe_psql('postgres', "SHOW wal_log_hints");
is($result, 'on', 'wal_log_hints changed to on via reload');

$node_primary->stop;

# Same pg_rewind now succeeds
command_ok(
	[
		'pg_rewind',
		'--target-pgdata' => $node_primary->data_dir,
		'--source-server' => $node_standby->connstr('postgres'),
		'--no-sync',
	],
	'pg_rewind succeeds after enabling wal_log_hints via reload');

$node_standby->stop;
done_testing();
