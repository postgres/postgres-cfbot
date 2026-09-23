# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Test for replication slots invalidation due to XID-age

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# Wait for the given slot to satisfy the given condition
sub wait_for_slot
{
	my ($node, $slot_name, $cond) = @_;
	$node->poll_query_until('postgres',
		"SELECT $cond FROM pg_replication_slots WHERE slot_name = '$slot_name'")
	  or die "Timed out waiting for slot $slot_name: $cond";
}

# Vacuum the given relation, then check the slot's invalidation reason and
# whether the relation's dead tuples could be removed.
sub vacuum_and_check
{
	my ($node, $relname, $slot_name, $reason, $dead_removed) = @_;

	$node->safe_psql('postgres', "VACUUM $relname");
	is( $node->safe_psql('postgres',
			"SELECT coalesce(invalidation_reason, 'none') FROM pg_replication_slots WHERE slot_name = '$slot_name'"
		),
		$reason,
		"slot $slot_name reads $reason after vacuuming $relname");
	is( $node->safe_psql('postgres',
			"SELECT n_dead_tup = 0 FROM pg_stat_all_tables WHERE relname = '$relname'"
		),
		$dead_removed ? 't' : 'f',
		"vacuum "
		  . ($dead_removed ? "removes" : "leaves")
		  . " the dead tuples in $relname");
}

# A small age lets slots reach the limit after just a few XIDs
my $slot_xid_age = 100;

# Consumes XIDs, one per committed transaction, to age a slot's xmin or
# catalog_xmin.
my $consume_xid_proc = qq{
	CREATE PROCEDURE consume_xid(cnt int)
	AS \$\$
	DECLARE
	    i int;
	BEGIN
	    FOR i IN 1..cnt LOOP
	        EXECUTE 'SELECT pg_current_xact_id()';
	        COMMIT;
	    END LOOP;
	END;
	\$\$ LANGUAGE plpgsql;
};

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical');

# No checkpoints, autovacuum or walsender timeouts, so that nothing invalidates
# a slot or advances its horizon behind a testcase's back.
$primary->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $slot_xid_age
autovacuum = off
checkpoint_timeout = 1h
wal_sender_timeout = 0
});
$primary->start;
$primary->safe_psql('postgres', $consume_xid_proc);
$primary->safe_psql('postgres',
	"CREATE TABLE tbl_user AS SELECT generate_series(1,10) AS a");

# Testcase 1: an inactive logical slot with an aged catalog_xmin is invalidated
# at a checkpoint.
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('logical_slot', 'pgoutput')");

# Note the log position before the slot ages, so no checkpoint can beat us to it
my $log_offset = -s $primary->logfile;
$primary->safe_psql('postgres', qq{CALL consume_xid(2 * $slot_xid_age)});
$primary->safe_psql('postgres', "CHECKPOINT");
wait_for_slot($primary, 'logical_slot', "invalidation_reason = 'xid_aged'");

# The slot holds a catalog_xmin alone, so only its age is reported
ok( $primary->log_contains(
		qr/invalidating obsolete replication slot "logical_slot"\n.*DETAIL:.*The slot's catalog xmin age of \d+ transactions exceeds the configured "max_slot_xid_age" of $slot_xid_age\./,
		$log_offset),
	'aged catalog_xmin is reported on invalidation');

$primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('logical_slot')");

# Testcase 2: a slot still being created holds an in-memory effective_xmin that
# is never written to disk. Such a slot shows no xmin in pg_replication_slots,
# but its age still counts.
my $running_xact = $primary->background_psql('postgres');
$running_xact->query_safe('BEGIN; SELECT pg_current_xact_id();');

# The open transaction keeps this slot from reaching a consistent point, so it
# stays in creation and keeps holding its xmin.
my $export = $primary->background_psql('postgres', replication => 'database');
$export->query_until(
	qr/create_started/, q(
\echo create_started
CREATE_REPLICATION_SLOT logical_export_slot LOGICAL pgoutput (SNAPSHOT 'export');
));
wait_for_slot($primary, 'logical_export_slot', 'catalog_xmin IS NOT NULL');

is( $primary->safe_psql('postgres',
		"SELECT xmin IS NULL FROM pg_replication_slots WHERE slot_name = 'logical_export_slot'"
	),
	't',
	'slot holding an effective xmin reports no xmin');

$log_offset = -s $primary->logfile;
$primary->safe_psql('postgres', qq{CALL consume_xid(2 * $slot_xid_age)});

# The slot is in use, so invalidation terminates its owner to release it
$primary->safe_psql('postgres', "CHECKPOINT");

# The slot holds both an xmin and a catalog_xmin, both aged, so the message
# reports both ages.
ok( $primary->log_contains(
		qr/terminating process \d+ to release replication slot "logical_export_slot"\n.*DETAIL:.*The slot's xmin age of \d+ transactions and catalog xmin age of \d+ transactions exceed the configured "max_slot_xid_age" of $slot_xid_age\./,
		$log_offset),
	'aged slot holding an effective xmin has its owner terminated');

# A slot still in creation is dropped, not invalidated, once its owner is gone
wait_for_slot($primary, 'logical_export_slot', 'count(*) = 0');

$running_xact->quit;

# The terminated backend took its psql down too, so just reap the process
$export->{run}->finish;

# Testcase 3: the VACUUM command skips an active logical slot with an aged
# catalog_xmin, rather than terminating its owner to invalidate it.
$primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('logical_active_slot', 'test_decoding')"
);

# Dead catalog rows that only this slot's catalog_xmin holds back
$primary->safe_psql('postgres',
	"CREATE TABLE tbl_tmp(a int); DROP TABLE tbl_tmp;");

# No status messages, so the client's feedback cannot advance the slot's
# catalog_xmin while the testcase ages it.
my ($stdout, $stderr);
my $recvlogical = IPC::Run::start(
	[
		'pg_recvlogical',
		'--dbname' => $primary->connstr('postgres'),
		'--slot' => 'logical_active_slot',
		'--status-interval' => 0,
		'--file' => '-',
		'--no-loop',
		'--start',
	],
	'>' => \$stdout,
	'2>' => \$stderr,
	IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));
wait_for_slot($primary, 'logical_active_slot', 'active_pid IS NOT NULL');

$primary->safe_psql('postgres', qq{CALL consume_xid(2 * $slot_xid_age)});

# Fail rather than pass vacuously, should the slot's horizon have moved anyway
is( $primary->safe_psql('postgres',
		"SELECT age(catalog_xmin) > $slot_xid_age FROM pg_replication_slots WHERE slot_name = 'logical_active_slot'"
	),
	't',
	'active slot is aged past the limit');

vacuum_and_check($primary, 'pg_class', 'logical_active_slot', 'none', 0);

# Testcase 4: the VACUUM command invalidates that same slot once it is
# inactive, and then removes the rows it was holding back.

# End the client's session to make the slot inactive (portable way)
$primary->safe_psql('postgres',
	"SELECT pg_terminate_backend(active_pid) FROM pg_replication_slots WHERE slot_name = 'logical_active_slot'"
);
wait_for_slot($primary, 'logical_active_slot', 'active_pid IS NULL');
$recvlogical->finish;

# A logical slot holds only a catalog_xmin, so a user table's cutoff is not its
# to hold back.
$primary->safe_psql('postgres', "VACUUM tbl_user");
is( $primary->safe_psql('postgres',
		"SELECT invalidation_reason IS NULL FROM pg_replication_slots WHERE slot_name = 'logical_active_slot'"
	),
	't',
	'logical slot not invalidated by vacuuming a user table');

vacuum_and_check($primary, 'pg_class', 'logical_active_slot', 'xid_aged', 1);
$primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('logical_active_slot')");

# Testcase 5: the VACUUM command invalidates an inactive physical slot with an
# aged xmin. Feedback from a standby is what gives such a slot an xmin, and
# stopping the standby freezes it.
my $backup_name = 'backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);

$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('phys_slot', true)");
$standby->append_conf(
	'postgresql.conf', q{
primary_slot_name = 'phys_slot'
hot_standby_feedback = on
wal_receiver_status_interval = 1
});
$standby->start;
$primary->wait_for_catchup($standby);
wait_for_slot($primary, 'phys_slot', 'xmin IS NOT NULL');
$standby->stop;

# Dead rows from an XID that the now frozen xmin holds back
$primary->safe_psql('postgres', "DELETE FROM tbl_user");

$log_offset = -s $primary->logfile;
$primary->safe_psql('postgres', qq{CALL consume_xid(2 * $slot_xid_age)});
vacuum_and_check($primary, 'tbl_user', 'phys_slot', 'xid_aged', 1);

# The slot holds an xmin alone, so only its age is reported
ok( $primary->log_contains(
		qr/invalidating obsolete replication slot "phys_slot"\n.*DETAIL:.*The slot's xmin age of \d+ transactions exceeds the configured "max_slot_xid_age" of $slot_xid_age\./,
		$log_offset),
	'aged xmin is reported on invalidation');

$primary->stop;

done_testing();
