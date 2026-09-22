# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests pg_xmin_horizon's replication_slot and standby_feedback rows.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 'logical');
$node_primary->start;

$node_primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('xmin_horizon_phys');");

my $backup_name = 'xmin_horizon_backup';
$node_primary->backup($backup_name);

# Slotted standby drives the replication_slot row.
my $node_slotted = PostgreSQL::Test::Cluster->new('standby_slotted');
$node_slotted->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_slotted->append_conf(
	'postgresql.conf', qq[
primary_slot_name = 'xmin_horizon_phys'
hot_standby_feedback = on
wal_receiver_status_interval = 1
]);
$node_slotted->start;

# Slot-less standby drives the standby_feedback row.
my $node_slotless = PostgreSQL::Test::Cluster->new('standby_slotless');
$node_slotless->init_from_backup($node_primary, $backup_name,
	has_streaming => 1);
$node_slotless->append_conf(
	'postgresql.conf', qq[
hot_standby_feedback = on
wal_receiver_status_interval = 1
]);
$node_slotless->start;

$node_primary->wait_for_catchup($node_slotted);
$node_primary->wait_for_catchup($node_slotless);

# (0) pg_xmin_horizon errors during recovery.  KnownAssignedXids
# is not surfaced as procarray rows, so the standby-local view alone would
# misattribute the horizon.
my $stderr;
$node_slotted->psql(
	'postgres',
	'SELECT * FROM pg_xmin_horizon LIMIT 1;',
	stderr => \$stderr);
ok( $stderr =~ /recovery is in progress/,
	'pg_xmin_horizon errors on a hot standby');

# Pin a stable xmin on each standby for hot_standby_feedback to relay.
my $psql_slotted = $node_slotted->background_psql('postgres');
$psql_slotted->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;");

my $psql_slotless = $node_slotless->background_psql('postgres');
$psql_slotless->query_safe(
	"BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;");

# (1) replication_slot row.
$node_primary->poll_query_until(
	'postgres',
	"SELECT xmin IS NOT NULL FROM pg_replication_slots WHERE slot_name = 'xmin_horizon_phys'",
	't')
  or die "timed out waiting for slot 'xmin_horizon_phys' to acquire an xmin";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot'
		   AND slot_name = 'xmin_horizon_phys'
		   AND data_xmin IS NOT NULL"),
	'1',
	'replication_slot row for xmin_horizon_phys carries an xmin');

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot'
		   AND slot_name = 'xmin_horizon_phys'
		   AND shared_xmin IS NOT NULL
		   AND shared_xmin = catalog_xmin
		   AND shared_xmin = data_xmin"),
	'1',
	'physical slot row reports the same non-null xmin in all three classes');

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot'
		   AND slot_name = 'xmin_horizon_phys'
		   AND datid IS NULL
		   AND xid IS NULL
		   AND xact_start IS NULL"),
	'1',
	'slot row has NULL datid, xid and xact_start');

# (2) standby_feedback row.  Exactly one row: the slot-less standby's
# walsender.  The slotted standby itself has no xmin (the slot carries it) and
# must NOT appear here.
$node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) > 0 FROM pg_xmin_horizon WHERE kind = 'standby_feedback'",
	't') or die "timed out waiting for a standby_feedback row to appear";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'standby_feedback'
		   AND data_xmin IS NOT NULL
		   AND datid IS NULL
		   AND xid IS NULL
		   AND xact_start IS NULL"),
	'1',
	'exactly one standby_feedback row with data_xmin set and NULL datid, xid and xact_start'
);

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon h
		 JOIN pg_stat_activity a ON a.pid = h.pid
		 WHERE h.kind = 'standby_feedback'
		   AND a.backend_type = 'walsender'"),
	'1',
	'standby_feedback pid is the walsender PID');

# (3) logical replication slot row: catalog_xmin non-null, data_xmin NULL.
$node_primary->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('xmin_horizon_log', 'test_decoding');"
);

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot'
		   AND slot_name = 'xmin_horizon_log'
		   AND catalog_xmin IS NOT NULL
		   AND shared_xmin = catalog_xmin
		   AND data_xmin IS NULL"),
	'1',
	'logical slot row reports catalog_xmin as shared_xmin and NULL data_xmin'
);

is( $node_primary->safe_psql(
		'postgres',
		"SELECT datid = (SELECT oid FROM pg_database WHERE datname = 'postgres')
		        AND datname = 'postgres'
		 FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot' AND slot_name = 'xmin_horizon_log'"),
	't',
	'logical slot row reports the decoded database in datid and datname');

$node_primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('xmin_horizon_log');");

# (4) pg_replication_slots reports data.xmin, but effective_xmin is what
# affects the horizon.  During slot creation with SNAPSHOT 'export', these
# values diverge, but pg_xmin_horizon must report the effective_xmin.  In
# this case, effective_xmin is set then discarded after slot creation
# completes, and data.xmin is never set.  Use a pre-existing transaction to
# block snapshot acquisition and freeze the slot in the divergent state.
my $psql_blocker = $node_primary->background_psql('postgres');
$psql_blocker->query_safe("BEGIN; SELECT pg_current_xact_id();");

my $psql_repl =
  $node_primary->background_psql('postgres', replication => 'database');
$psql_repl->query_until(
	qr/create_started/, q(
\echo create_started
CREATE_REPLICATION_SLOT xmin_horizon_export LOGICAL test_decoding (SNAPSHOT 'export');
));

$node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) = 1 FROM pg_xmin_horizon
	 WHERE kind = 'replication_slot'
	   AND slot_name = 'xmin_horizon_export'
	   AND data_xmin IS NOT NULL",
	't') or die "timed out waiting for effective_xmin";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT h.data_xmin IS NOT NULL AND h.catalog_xmin IS NOT NULL
		        AND s.xmin IS NULL AND s.catalog_xmin IS NOT NULL
		 FROM pg_xmin_horizon h
		 JOIN pg_replication_slots s ON s.slot_name = h.slot_name
		 WHERE h.kind = 'replication_slot'
		   AND h.slot_name = 'xmin_horizon_export'"),
	't',
	'data_xmin set during blocked slot creation while pg_replication_slots shows NULL'
);

# End the blocking transaction and let the creation finish.  The snapshot
# builder needs a running-xacts record that no longer contains the xid, so
# log one on every poll iteration.
$psql_blocker->query_safe("COMMIT");
$psql_blocker->quit;

$node_primary->poll_query_until(
	'postgres',
	"SELECT pg_log_standby_snapshot() IS NOT NULL
	    AND (SELECT active_pid IS NULL FROM pg_replication_slots
	         WHERE slot_name = 'xmin_horizon_export')",
	't') or die "timed out waiting for slot creation to finish";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT data_xmin IS NULL AND catalog_xmin IS NOT NULL
		 FROM pg_xmin_horizon
		 WHERE kind = 'replication_slot'
		   AND slot_name = 'xmin_horizon_export'"),
	't',
	'effective_xmin discarded but catalog_xmin kept after slot creation completes'
);

$psql_repl->quit;
$node_primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('xmin_horizon_export');");

# (5) A backend row's datid reflects its connection.
$node_primary->safe_psql('postgres', 'CREATE DATABASE xmin_horizon_db2;');

my $psql_db2 = $node_primary->background_psql('xmin_horizon_db2');
$psql_db2->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1;");

$node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) > 0 FROM pg_xmin_horizon
	 WHERE kind = 'backend'
	   AND datid = (SELECT oid FROM pg_database WHERE datname = 'xmin_horizon_db2')
	   AND data_xmin IS NOT NULL",
	't') or die "timed out waiting for xmin_horizon_db2 backend xmin";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'backend'
		   AND datid = (SELECT oid FROM pg_database WHERE datname = 'xmin_horizon_db2')
		   AND datname = 'xmin_horizon_db2'
		   AND data_xmin IS NOT NULL"),
	'1',
	'backend in xmin_horizon_db2 appears with the right datid and datname');

$psql_db2->quit;
$node_primary->safe_psql('postgres', 'DROP DATABASE xmin_horizon_db2;');

# (6) Tied backend rows via shared exported snapshot.  Tear down older-xmin
# holders first so the tied pair is the global min.
$psql_slotted->quit;
$psql_slotless->quit;
$node_slotless->stop;
$node_slotted->stop;
$node_primary->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('xmin_horizon_phys')");

my $psql_tie_a = $node_primary->background_psql('postgres');
my $psql_tie_b = $node_primary->background_psql('postgres');

$psql_tie_a->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ");
my $snap_id = $psql_tie_a->query_safe("SELECT pg_export_snapshot()");
chomp $snap_id;
my $tie_pid_a = $psql_tie_a->query_safe("SELECT pg_backend_pid()");
chomp $tie_pid_a;

$psql_tie_b->query_safe("BEGIN ISOLATION LEVEL REPEATABLE READ");
$psql_tie_b->query_safe("SET TRANSACTION SNAPSHOT '$snap_id'");
my $tie_pid_b = $psql_tie_b->query_safe("SELECT pg_backend_pid()");
chomp $tie_pid_b;

$node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) = 2 FROM pg_xmin_horizon
	 WHERE kind = 'backend' AND pid IN ($tie_pid_a, $tie_pid_b)
	   AND data_xmin IS NOT NULL",
	't') or die "timed out waiting for tied backends' xmin";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT (SELECT data_xmin FROM pg_xmin_horizon
		         WHERE kind = 'backend' AND pid = $tie_pid_a)
		      = (SELECT data_xmin FROM pg_xmin_horizon
		         WHERE kind = 'backend' AND pid = $tie_pid_b)"),
	't',
	'tied backends share data_xmin');

$psql_tie_a->quit;
$psql_tie_b->quit;

# (7) VACUUM backend row: PROC_IN_VACUUM is set with all per-class xmins
# NULL.  Parking VACUUM on a conflicting lock holds it in that state while
# the assertion runs.
$node_primary->safe_psql('postgres',
	"CREATE UNLOGGED TABLE xmin_horizon_vac_t (id int);");

# Locker holds SHARE on the target so VACUUM's ShareUpdateExclusiveLock
# acquisition blocks.
my $psql_locker = $node_primary->background_psql('postgres');
$psql_locker->query_safe(
	"BEGIN; LOCK TABLE xmin_horizon_vac_t IN SHARE MODE;");

# VACUUM session: capture pid first, then fire VACUUM via query_until so
# the main thread proceeds while VACUUM is still blocked.
my $psql_vacuum = $node_primary->background_psql('postgres');
my $vac_pid = $psql_vacuum->query_safe("SELECT pg_backend_pid()");
chomp $vac_pid;

$psql_vacuum->query_until(
	qr/vacuum_started/, q(
\echo vacuum_started
VACUUM xmin_horizon_vac_t;
));

# The VACUUM backend's row appears once its snapshot is pushed, which sets
# its xmin.  By then, PROC_IN_VACUUM is already set and the proc is blocked
# acquiring the ShareUpdateExclusiveLock.
$node_primary->poll_query_until(
	'postgres',
	"SELECT count(*) = 1 FROM pg_xmin_horizon
	 WHERE kind = 'backend' AND pid = $vac_pid",
	't') or die "timed out waiting for VACUUM backend row";

is( $node_primary->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_xmin_horizon
		 WHERE kind = 'backend' AND pid = $vac_pid
		   AND shared_xmin IS NULL
		   AND catalog_xmin IS NULL
		   AND data_xmin IS NULL"),
	'1',
	'VACUUM backend row shows NULL in all three xmin classes');

# Release the lock so VACUUM completes, then drain both sessions.
$psql_locker->query_safe("COMMIT");
$psql_locker->quit;
$psql_vacuum->quit;

$node_primary->safe_psql('postgres', "DROP TABLE xmin_horizon_vac_t");

$node_primary->stop;

done_testing();
