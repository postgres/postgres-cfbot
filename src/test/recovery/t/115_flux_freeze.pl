# Copyright (c) 2026, PostgreSQL Global Development Group
# CLOG truncation must not invalidate frozen rows or UNDO before-images.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
vacuum_freeze_min_age = 0
wal_consistency_checking = all
full_page_writes = off
statement_timeout = '120s'
});
$node->start;
$node->safe_psql(
	'postgres',
	'CREATE EXTENSION amcheck; CREATE EXTENSION xid_wraparound; CREATE EXTENSION injection_points;
ALTER DATABASE template0 ALLOW_CONNECTIONS true;');
for my $am (qw(heap flux))
{
	$node->safe_psql(
		'postgres', qq{
CREATE TABLE frozen_$am (id int PRIMARY KEY, v int) USING $am;
CREATE TABLE chain_$am (id int PRIMARY KEY, v int) USING $am;
INSERT INTO frozen_$am VALUES (1,10);
INSERT INTO chain_$am VALUES (1,10);
});
}
my $old_xid = $node->safe_psql('postgres', 'SELECT xmin FROM chain_flux');
my $old_segment = sprintf('%04X', int($old_xid / (8192 * 4 * 32)));
ok(-f $node->data_dir . "/pg_xact/$old_segment",
	'original CLOG segment exists');
$node->safe_psql('postgres', 'SELECT consume_xids(2200000)');
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->backup('base');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'base', has_streaming => 1);
$standby->append_conf('postgresql.conf',
	"wal_consistency_checking = all\nmax_standby_streaming_delay = 0\n");
$standby->start;
my $reader = $node->background_psql('postgres');
$reader->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_current_snapshot();');
# Do not read the old row here: doing so could set the missing commit hint.
for my $am (qw(heap flux))
{
	$node->safe_psql('postgres', "UPDATE chain_$am SET v=20;");
}
my $before = $node->safe_psql('postgres',
	'SELECT datfrozenxid::text::bigint FROM pg_database WHERE datname=current_database()'
);
for my $db (qw(template0 template1 postgres))
{
	$node->safe_psql($db, 'VACUUM FREEZE');
}
$node->safe_psql('postgres', 'CHECKPOINT');
my $after = $node->safe_psql('postgres',
	'SELECT datfrozenxid::text::bigint FROM pg_database WHERE datname=current_database()'
);
cmp_ok($after, '>', $before, 'database freeze horizon advances');
ok( !-f $node->data_dir . "/pg_xact/$old_segment",
	'original CLOG segment actually truncated');
for my $am (qw(heap flux))
{
	is($reader->query_safe("SELECT v FROM chain_$am"),
		'10', "$am old snapshot reads before-image after truncation");
	is($node->safe_psql('postgres', "SELECT v FROM frozen_$am"),
		'10', "$am frozen row survives truncation");
}
$reader->query_safe('COMMIT');
$reader->quit;
$node->wait_for_replay_catchup($standby);
for my $am (qw(heap flux))
{
	is($standby->safe_psql('postgres', "SELECT v FROM frozen_$am"),
		'10', "$am standby frozen row");
	is($standby->safe_psql('postgres', "SELECT v FROM chain_$am"),
		'20', "$am standby current version");
	for my $n ($node, $standby)
	{
		is( $n->safe_psql(
				'postgres',
				"SELECT bt_index_check('frozen_${am}_pkey'); SELECT bt_index_check('chain_${am}_pkey');"
			),
			"\n",
			"$am indexes pass amcheck on " . $n->name);
	}
}
# A non-freezing VACUUM setting ALL_VISIBLE must also resolve conflicts.
my $vm_log_offset = -s $standby->logfile;
my $vm_reader = $standby->background_psql('postgres', on_error_stop => 0);
$vm_reader->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_current_snapshot();');
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'INSERT INTO frozen_flux VALUES (2,30)');
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->safe_psql('postgres',
	'SET vacuum_freeze_min_age=100000000; VACUUM frozen_flux');
is( $node->safe_psql(
		'postgres',
		"SELECT relallvisible FROM pg_class WHERE oid='frozen_flux'::regclass"
	),
	'1',
	'nonfreezing vacuum made the page all-visible');
# VACUUM's last VM record need not yet be flushed. Flush before waiting.
$node->safe_psql('postgres', 'CHECKPOINT');
$node->wait_for_replay_catchup($standby);
eval { $vm_reader->query('SELECT v FROM frozen_flux WHERE id=2'); };
# The standby logs the cancellation; the client may only see a closed
# connection (Windows), so check the standby's log rather than its stderr.
ok( $vm_reader->{stderr} =~ /conflict with recovery/
	  || $standby->log_contains(qr/conflict with recovery/, $vm_log_offset),
	'all-visible WAL resolves older standby snapshots');
eval { $vm_reader->quit; };
# A DML-normalized before-image must cancel an older standby snapshot.
# Disable feedback so the primary can advance beyond that snapshot.
my $old_log_offset = -s $standby->logfile;
my $old_standby = $standby->background_psql('postgres', on_error_stop => 0);
$old_standby->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT pg_current_snapshot();');
$node->safe_psql('postgres', 'INSERT INTO chain_flux VALUES (2,30)');
$node->safe_psql('postgres', 'UPDATE chain_flux SET v=40 WHERE id=2');
$node->wait_for_replay_catchup($standby);
eval { $old_standby->query('SELECT v FROM chain_flux WHERE id=2'); };
ok( $old_standby->{stderr} =~ /conflict with recovery/
	  || $standby->log_contains(qr/conflict with recovery/, $old_log_offset),
	'before-image freezing resolves older standby snapshots');
eval { $old_standby->quit; };

for my $am (qw(heap flux))
{
	$node->safe_psql('postgres',
		"CREATE TABLE parent_$am (id int PRIMARY KEY) USING $am; CREATE TABLE child_$am (id int PRIMARY KEY, p int REFERENCES parent_$am(id) DEFERRABLE INITIALLY DEFERRED) USING $am; INSERT INTO parent_$am VALUES (1); INSERT INTO child_$am VALUES (1,1);"
	);
	is( $node->safe_psql(
			'postgres',
			"BEGIN; UPDATE child_$am SET p=1; UPDATE child_$am SET p=1; SET CONSTRAINTS ALL IMMEDIATE; COMMIT; SELECT count(*) FROM child_$am"
		),
		'1',
		"$am deferred checks survive normalized before-images");
}
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');"
);
$node->safe_psql('postgres',
	"BEGIN; DELETE FROM frozen_heap WHERE id=1; DELETE FROM frozen_flux WHERE id=1; ROLLBACK;"
);
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
$node->safe_psql('postgres',
	'SELECT consume_xids(100); SELECT pg_current_xact_id()');
for my $am (qw(heap flux))
{
	$node->safe_psql('postgres', "VACUUM (FREEZE) frozen_$am");
	is($node->safe_psql('postgres', "SELECT v FROM frozen_$am WHERE id=1"),
		'10', "$am VACUUM preserves a row with an aborted delete");
	is( $node->safe_psql(
			'postgres',
			"SELECT age(relfrozenxid) <= 1 FROM pg_class WHERE oid='frozen_$am'::regclass"
		),
		't',
		"$am aborted delete does not pin freezing");
}
$node->safe_psql('postgres',
	"SELECT injection_points_detach('pbu-undo-worker-before-apply'); SELECT injection_points_wakeup('pbu-undo-worker-before-apply');"
);
$standby->stop;
$node->stop('immediate');
$node->start;
for my $am (qw(heap flux))
{
	is($node->safe_psql('postgres', "SELECT v FROM frozen_$am WHERE id=1"),
		'10', "$am frozen row survives crash recovery");
	$node->safe_psql('postgres',
		"BEGIN; UPDATE frozen_$am SET v=99 WHERE id=1; ROLLBACK;");
	is($node->safe_psql('postgres', "SELECT v FROM frozen_$am WHERE id=1"),
		'10', "$am rollback restores a frozen row");
}
$node->stop;
done_testing();
