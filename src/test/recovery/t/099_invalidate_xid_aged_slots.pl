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

# No checkpoints and no autovacuum, so that a slot is invalidated only where a
# testcase asks for it.
$primary->append_conf(
	'postgresql.conf', qq{
max_slot_xid_age = $slot_xid_age
autovacuum = off
checkpoint_timeout = 1h
});
$primary->start;
$primary->safe_psql('postgres', $consume_xid_proc);

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

$primary->stop;

done_testing();
