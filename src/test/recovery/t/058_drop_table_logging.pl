# Copyright (c) 2026, PostgreSQL Global Development Group

# Test DROP TABLE, TRUNCATE TABLE, and DROP DATABASE logging functionality.
# Verifies that operations are logged with correct LSN values, and that
# point-in-time recovery (PITR) to the logged LSN actually recovers the data.

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Initialize primary node with WAL archiving and streaming enabled for PITR
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->append_conf('postgresql.conf', qq{
log_min_messages = log
logging_collector = off
log_destination = 'stderr'
log_object_drops = on
max_prepared_transactions = 5
});
$node->start;

# Track log file position for incremental reading
my $log_offset = 0;

# Cache for current test - stores log content read once per test
my $current_test_log_cache = undef;

# Reset to start a new test - clears cache and updates offset
sub start_new_test
{
	my ($test_name) = @_;
	note($test_name) if defined $test_name;

	$current_test_log_cache = undef;

	# Update offset to current position
	my $logfile = $node->logfile;
	$log_offset = -s $logfile;
}

# Get log content for current test (cached within test)
sub get_test_log_content
{
	return $current_test_log_cache if defined $current_test_log_cache;

	# Read new content since last test started
	my $logfile = $node->logfile;
	my $current_size = -s $logfile;

	# If offset is beyond file size, reset to 0
	$log_offset = 0 if $log_offset > $current_size;

	# Read only new content
	$current_test_log_cache = slurp_file($logfile, $log_offset);

	return $current_test_log_cache;
}

# Count matching log entries in current test
sub count_drop_logs
{
	my ($pattern) = @_;
	my $log = get_test_log_content();
	my @matches = $log =~ /$pattern/g;
	return scalar @matches;
}

# Get matching log lines in current test
sub get_log_lines
{
	my ($pattern) = @_;
	my $log = get_test_log_content();
	my @lines = split /\n/, $log;
	my @matching_lines = grep { /$pattern/ } @lines;
	return @matching_lines;
}

# Helper function to extract LSN from log line
sub extract_lsn
{
	my ($line) = @_;
	if ($line =~ /lsn[:=]\s*([0-9A-Fa-f]+\/[0-9A-Fa-f]+)/i)
	{
		return $1;
	}
	return undef;
}

# Build pattern for DROP TABLE log entry
sub drop_table_pattern
{
	my ($schema, $table) = @_;
	return qr/table "$schema\.$table" \(OID \d+\) dropped/;
}

# Build pattern for TRUNCATE TABLE log entry
sub truncate_table_pattern
{
	my ($schema, $table) = @_;
	return qr/table "$schema\.$table" \(OID \d+\) truncated/;
}

# Build pattern for DROP DATABASE log entry
sub drop_database_pattern
{
	my ($dbname) = @_;
	return qr/database "$dbname" \(OID \d+\) dropped/;
}

# Test helper: Execute SQL and verify drop count
sub test_drop_count
{
	my ($test_name, $sql, $pattern, $expected_count) = @_;

	start_new_test($test_name);
	$node->safe_psql('postgres', $sql);

	my $count = count_drop_logs($pattern);
	is($count, $expected_count, "$test_name: count check");
}

# Test helper: Execute SQL and verify log entry exists with valid LSN
sub test_drop_logged
{
	my ($test_name, $sql, $pattern, $extra_checks) = @_;

	start_new_test($test_name);
	$node->safe_psql('postgres', $sql);

	my @log_lines = get_log_lines($pattern);
	is(scalar @log_lines, 1, "$test_name: entry logged");

	if (@log_lines)
	{
		like($log_lines[0], qr/lsn[:=]\s*[0-9A-Fa-f]+\/[0-9A-Fa-f]+/i,
		     "$test_name: LSN present");
		unlike($log_lines[0], qr/lsn[:=]\s*0\/0\b/i,
		       "$test_name: LSN not invalid");

		# Execute additional checks if provided
		$extra_checks->($log_lines[0]) if defined $extra_checks;
	}
}

# Test helper: Execute SQL and verify nothing was logged
sub test_drop_not_logged
{
	my ($test_name, $sql, $pattern) = @_;

	start_new_test($test_name);
	$node->safe_psql('postgres', $sql);

	my $count = count_drop_logs($pattern);
	is($count, 0, "$test_name: not logged");
}

# Test helper: Verify multiple drops in one transaction
sub test_multiple_drops
{
	my ($test_name, $sql, @table_specs) = @_;

	start_new_test($test_name);
	$node->safe_psql('postgres', $sql);

	foreach my $spec (@table_specs)
	{
		my ($schema, $table, $expected) = @$spec;
		my $count = count_drop_logs(drop_table_pattern($schema, $table));
		is($count, $expected, "$test_name: $schema.$table");
	}
}

# Test helper: Verify multiple truncates in one transaction
sub test_multiple_truncates
{
	my ($test_name, $sql, @table_specs) = @_;

	start_new_test($test_name);
	$node->safe_psql('postgres', $sql);

	foreach my $spec (@table_specs)
	{
		my ($schema, $table, $expected) = @$spec;
		my $count = count_drop_logs(truncate_table_pattern($schema, $table));
		is($count, $expected, "$test_name: $schema.$table");
	}
}

# ==============================================================================
# PITR Tests: Verify that recovering to the logged LSN actually restores data
# ==============================================================================

# Take a base backup before testing PITR
$node->backup('bkp');

# PITR Test 1: DROP TABLE recovery
start_new_test('PITR recovery for DROP TABLE');
$node->safe_psql('postgres', q{
	CREATE TABLE pitr_drop_table (id int, val text);
	INSERT INTO pitr_drop_table VALUES (1, 'alpha'), (2, 'beta');
});
$node->safe_psql('postgres', q{
	DROP TABLE pitr_drop_table;
});
my @drop_lines = get_log_lines(drop_table_pattern('public', 'pitr_drop_table'));
is(scalar @drop_lines, 1, 'PITR: DROP TABLE logged');
my $drop_lsn = extract_lsn($drop_lines[0]);
ok(defined $drop_lsn, "PITR: Scraped drop commit LSN: $drop_lsn");

# Switch WAL to make sure the commit record is archived
my $walfile_drop = $node->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn());");
$node->safe_psql('postgres', "SELECT pg_switch_wal();");
$node->poll_query_until('postgres',
	"SELECT '$walfile_drop' <= last_archived_wal FROM pg_stat_archiver;")
  or die "Timed out waiting for WAL archival";

# Restore a standby node to the scraped LSN with recovery_target_inclusive = false
my $node_pitr_drop = PostgreSQL::Test::Cluster->new('pitr_drop');
$node_pitr_drop->init_from_backup($node, 'bkp', has_restoring => 1, standby => 0);
$node_pitr_drop->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$drop_lsn'
recovery_target_inclusive = false
recovery_target_action = 'promote'
});
$node_pitr_drop->start;
$node_pitr_drop->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out waiting for PITR promotion after DROP TABLE";

my $drop_count = $node_pitr_drop->safe_psql('postgres',
	"SELECT count(*) FROM pitr_drop_table;");
is($drop_count, '2', 'PITR DROP: Table exists with all 2 rows restored');
my $drop_vals = $node_pitr_drop->safe_psql('postgres',
	"SELECT string_agg(val, ',' ORDER BY id) FROM pitr_drop_table;");
is($drop_vals, 'alpha,beta', 'PITR DROP: Table row contents match exactly');
$node_pitr_drop->teardown_node;


# PITR Test 2: TRUNCATE TABLE recovery
start_new_test('PITR recovery for TRUNCATE TABLE');
$node->safe_psql('postgres', q{
	CREATE TABLE pitr_trunc_table (id int, val text);
	INSERT INTO pitr_trunc_table VALUES (1, 'foo'), (2, 'bar'), (3, 'baz');
});
$node->safe_psql('postgres', q{
	TRUNCATE TABLE pitr_trunc_table;
});
my @trunc_lines = get_log_lines(truncate_table_pattern('public', 'pitr_trunc_table'));
is(scalar @trunc_lines, 1, 'PITR: TRUNCATE TABLE logged');
my $trunc_lsn = extract_lsn($trunc_lines[0]);
ok(defined $trunc_lsn, "PITR: Scraped truncate commit LSN: $trunc_lsn");

# Switch WAL to make sure the commit record is archived
my $walfile_trunc = $node->safe_psql('postgres',
	"SELECT pg_walfile_name(pg_current_wal_lsn());");
$node->safe_psql('postgres', "SELECT pg_switch_wal();");
$node->poll_query_until('postgres',
	"SELECT '$walfile_trunc' <= last_archived_wal FROM pg_stat_archiver;")
  or die "Timed out waiting for WAL archival";

# Restore a standby node to the scraped LSN with recovery_target_inclusive = false
my $node_pitr_trunc = PostgreSQL::Test::Cluster->new('pitr_trunc');
$node_pitr_trunc->init_from_backup($node, 'bkp', has_restoring => 1, standby => 0);
$node_pitr_trunc->append_conf('postgresql.conf', qq{
recovery_target_lsn = '$trunc_lsn'
recovery_target_inclusive = false
recovery_target_action = 'promote'
});
$node_pitr_trunc->start;
$node_pitr_trunc->poll_query_until('postgres', "SELECT pg_is_in_recovery() = 'f';")
  or die "Timed out waiting for PITR promotion after TRUNCATE TABLE";

my $trunc_count = $node_pitr_trunc->safe_psql('postgres',
	"SELECT count(*) FROM pitr_trunc_table;");
is($trunc_count, '3', 'PITR TRUNCATE: All 3 rows preserved');
my $trunc_vals = $node_pitr_trunc->safe_psql('postgres',
	"SELECT string_agg(val, ',' ORDER BY id) FROM pitr_trunc_table;");
is($trunc_vals, 'foo,bar,baz', 'PITR TRUNCATE: Table row contents match exactly');
$node_pitr_trunc->teardown_node;


# ==============================================================================
# Functional Tests
# ==============================================================================

# Test 1: Single statement DROP TABLE
test_drop_logged(
	'Test 1: Simple DROP TABLE',
	q{
		CREATE TABLE test_simple (id int);
		INSERT INTO test_simple VALUES (1);
		DROP TABLE test_simple;
	},
	drop_table_pattern('public', 'test_simple')
);

# Test 2: DROP TABLE inside transaction block
test_drop_logged(
	'Test 2: DROP TABLE in transaction',
	q{
		CREATE TABLE test_in_xact (id int);
		INSERT INTO test_in_xact VALUES (1);
		BEGIN;
		DROP TABLE test_in_xact;
		COMMIT;
	},
	drop_table_pattern('public', 'test_in_xact')
);

# Test 3a: ROLLBACK of DROP TABLE (should NOT be logged)
test_drop_not_logged(
	'Test 3a: Rolled back DROP not logged',
	q{
		CREATE TABLE test_rollback (id int);
		INSERT INTO test_rollback VALUES (1);
		BEGIN;
		DROP TABLE test_rollback;
		ROLLBACK;
	},
	drop_table_pattern('public', 'test_rollback')
);

# Test 3b: Committed DROP after rollback
test_drop_count(
	'Test 3b: Committed DROP logged',
	q{
		SELECT * FROM test_rollback;
		DROP TABLE test_rollback;
	},
	drop_table_pattern('public', 'test_rollback'),
	1
);

# Test 4: DROP SCHEMA CASCADE - all tables logged
test_multiple_drops(
	'Test 4: DROP SCHEMA CASCADE - all tables logged',
	q{
		CREATE SCHEMA test_schema;
		CREATE TABLE test_schema.table1 (id int);
		CREATE TABLE test_schema.table2 (name text);
		INSERT INTO test_schema.table1 VALUES (1);
		INSERT INTO test_schema.table2 VALUES ('test');
		BEGIN;
		DROP SCHEMA test_schema CASCADE;
		COMMIT;
	},
	['test_schema', 'table1', 1],
	['test_schema', 'table2', 1]
);

# Test 5: DROP TABLE with FK CASCADE - only explicitly dropped table is logged
test_drop_count(
	'Test 5: DROP TABLE CASCADE with foreign keys',
	q{
		CREATE TABLE test_parent (id int PRIMARY KEY);
		CREATE TABLE test_child (id int, parent_id int REFERENCES test_parent(id));
		INSERT INTO test_parent VALUES (1);
		INSERT INTO test_child VALUES (1, 1);
		BEGIN;
		DROP TABLE test_parent CASCADE;
		COMMIT;
	},
	drop_table_pattern('public', 'test_parent'),
	1
);

# Test 6: Multiple DROP TABLE in single statement
test_multiple_drops(
	'Test 6: Multiple tables in single DROP statement',
	q{
		CREATE TABLE test_multi1 (id int);
		CREATE TABLE test_multi2 (id int);
		CREATE TABLE test_multi3 (id int);
		BEGIN;
		DROP TABLE test_multi1, test_multi2, test_multi3;
		COMMIT;
	},
	['public', 'test_multi1', 1],
	['public', 'test_multi2', 1],
	['public', 'test_multi3', 1]
);
my @hint_lines = get_log_lines(qr/HINT:\s+To recover dropped or truncated tables/);
is(scalar @hint_lines, 1, 'Test 6: Exactly one hint emitted for multiple table drops');

# Test 7: DROP PARTITIONED TABLE - all tables logged
test_multiple_drops(
	'Test 7: Partitioned table and partitions',
	q{
		CREATE TABLE test_partitioned (id int, created_at date) PARTITION BY RANGE (created_at);
		CREATE TABLE test_part_2024 PARTITION OF test_partitioned
		    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
		CREATE TABLE test_part_2025 PARTITION OF test_partitioned
		    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
		BEGIN;
		DROP TABLE test_partitioned CASCADE;
		COMMIT;
	},
	['public', 'test_partitioned', 1],
	['public', 'test_part_2024', 1],
	['public', 'test_part_2025', 1]
);

# Test 7b: Explicit DROP of single child partition directly is logged
test_drop_logged(
	'Test 7b: Direct DROP of child partition',
	q{
		CREATE TABLE test_part_p2 (id int) PARTITION BY RANGE (id);
		CREATE TABLE test_part_c1 PARTITION OF test_part_p2 FOR VALUES FROM (1) TO (10);
		CREATE TABLE test_part_c2 PARTITION OF test_part_p2 FOR VALUES FROM (10) TO (20);
		DROP TABLE test_part_c1;
		DROP TABLE test_part_p2;
	},
	drop_table_pattern('public', 'test_part_c1')
);

# Test 7c: DROP SCHEMA CASCADE with partition whose parent is in another schema
test_multiple_drops(
	'Test 7c: DROP SCHEMA with external partition',
	q{
		CREATE SCHEMA schema_parent;
		CREATE SCHEMA schema_part;
		CREATE TABLE schema_parent.parent (id int) PARTITION BY RANGE (id);
		CREATE TABLE schema_part.part1 PARTITION OF schema_parent.parent FOR VALUES FROM (1) TO (10);
		BEGIN;
		DROP SCHEMA schema_part CASCADE;
		COMMIT;
		DROP TABLE schema_parent.parent;
	},
	['schema_part', 'part1', 1]
);

# Test 8: Mixed operations in one transaction
test_multiple_drops(
	'Test 8: Mixed CREATE and DROP operations in transaction',
	q{
		CREATE SCHEMA mixed_schema;
		BEGIN;
		CREATE TABLE mixed_schema.table1 (id int);
		INSERT INTO mixed_schema.table1 VALUES (1);
		CREATE TABLE mixed_schema.table2 (id int);
		DROP TABLE mixed_schema.table2;
		CREATE TABLE outside_table (id int);
		INSERT INTO outside_table VALUES (1);
		DROP SCHEMA mixed_schema CASCADE;
		DROP TABLE outside_table;
		COMMIT;
	},
	['mixed_schema', 'table1', 1],
	['mixed_schema', 'table2', 1],
	['public', 'outside_table', 1]
);

# Test 9: Disabled GUC (log_object_drops = off)
test_drop_not_logged(
	'Test 9: log_object_drops = off does not log',
	q{
		SET log_object_drops = off;
		CREATE TABLE test_guc_disabled (id int);
		DROP TABLE test_guc_disabled;
		SET log_object_drops = on;
	},
	drop_table_pattern('public', 'test_guc_disabled')
);

# Test 10: DROP temporary table (should NOT be logged)
test_drop_not_logged(
	'Test 10: Temporary table',
	q{
		CREATE TEMP TABLE test_temp (id int);
		INSERT INTO test_temp VALUES (1);
		BEGIN;
		DROP TABLE test_temp;
		COMMIT;
	},
	drop_table_pattern('pg_temp', 'test_temp')
);

# Test 11: DROP UNLOGGED table (should NOT be logged)
test_drop_not_logged(
	'Test 11: UNLOGGED table',
	q{
		CREATE UNLOGGED TABLE test_unlogged (id int);
		INSERT INTO test_unlogged VALUES (1);
		BEGIN;
		DROP TABLE test_unlogged;
		COMMIT;
	},
	drop_table_pattern('public', 'test_unlogged')
);

# Test 12: DROP VIEW (should NOT be logged)
test_drop_not_logged(
	'Test 12: VIEW drop not logged',
	q{
		CREATE TABLE test_view_base (id int);
		CREATE VIEW test_view AS SELECT * FROM test_view_base;
		DROP VIEW test_view;
		DROP TABLE test_view_base;
	},
	qr/table ".*test_view" \(OID \d+\) dropped/
);

# Test 13: DROP MATERIALIZED VIEW (should NOT be logged)
test_drop_not_logged(
	'Test 13: MATERIALIZED VIEW drop not logged',
	q{
		CREATE MATERIALIZED VIEW test_matview AS SELECT 1 AS a;
		DROP MATERIALIZED VIEW test_matview;
	},
	qr/table ".*test_matview" \(OID \d+\) dropped/
);

# Test 14: DROP INDEX (should NOT be logged)
test_drop_not_logged(
	'Test 14: INDEX drop not logged',
	q{
		CREATE TABLE test_index_table (id int);
		CREATE INDEX test_idx ON test_index_table(id);
		DROP INDEX test_idx;
		DROP TABLE test_index_table;
	},
	qr/table ".*test_idx" \(OID \d+\) dropped/
);

# Test 15: Table rewrite (ALTER TABLE ... ALTER COLUMN TYPE)
test_drop_not_logged(
	'Test 15: Table rewrite does not produce false DROP TABLE log',
	q{
		CREATE TABLE test_rewrite (id int);
		INSERT INTO test_rewrite VALUES (1);
		ALTER TABLE test_rewrite ALTER COLUMN id TYPE bigint;
		DROP TABLE test_rewrite;
	},
	qr/table ".*pg_temp.*" \(OID \d+\) dropped/
);

# Test 16: Table inheritance hierarchy - all tables logged
test_multiple_drops(
	'Test 16: Inheritance hierarchy',
	q{
		CREATE TABLE parent_inherit (id int);
		CREATE TABLE child_inherit1 () INHERITS (parent_inherit);
		CREATE TABLE child_inherit2 () INHERITS (parent_inherit);
		INSERT INTO parent_inherit VALUES (1);
		INSERT INTO child_inherit1 VALUES (2);
		INSERT INTO child_inherit2 VALUES (3);
		BEGIN;
		DROP TABLE parent_inherit CASCADE;
		COMMIT;
	},
	['public', 'parent_inherit', 1],
	['public', 'child_inherit1', 1],
	['public', 'child_inherit2', 1]
);

# Test 17: SAVEPOINT and ROLLBACK TO
start_new_test('Test 17: SAVEPOINT and ROLLBACK TO');
$node->safe_psql('postgres', q{
	CREATE TABLE test_savepoint1 (id int);
	CREATE TABLE test_savepoint2 (id int);
	CREATE TABLE test_savepoint3 (id int);
	BEGIN;
	DROP TABLE test_savepoint1;
	SAVEPOINT sp1;
	DROP TABLE test_savepoint2;
	ROLLBACK TO sp1;
	DROP TABLE test_savepoint3;
	COMMIT;
});
is(count_drop_logs(drop_table_pattern('public', 'test_savepoint1')), 1, 'Test 17: sp1 before savepoint');
is(count_drop_logs(drop_table_pattern('public', 'test_savepoint2')), 0, 'Test 17: sp2 rolled back');
is(count_drop_logs(drop_table_pattern('public', 'test_savepoint3')), 1, 'Test 17: sp3 after rollback');
$node->safe_psql('postgres', 'DROP TABLE IF EXISTS test_savepoint2;');

# Test 18: SAVEPOINT and RELEASE
start_new_test('Test 18: SAVEPOINT and RELEASE');
$node->safe_psql('postgres', q{
	CREATE TABLE test_release1 (id int);
	CREATE TABLE test_release2 (id int);
	BEGIN;
	DROP TABLE test_release1;
	SAVEPOINT sp1;
	DROP TABLE test_release2;
	RELEASE SAVEPOINT sp1;
	COMMIT;
});
is(count_drop_logs(drop_table_pattern('public', 'test_release1')), 1, 'Test 18: release1 logged');
is(count_drop_logs(drop_table_pattern('public', 'test_release2')), 1, 'Test 18: release2 logged');

# Test 19: Nested SAVEPOINTs
start_new_test('Test 19: Nested SAVEPOINTs');
$node->safe_psql('postgres', q{
	CREATE TABLE test_nested1 (id int);
	CREATE TABLE test_nested2 (id int);
	CREATE TABLE test_nested3 (id int);
	BEGIN;
	SAVEPOINT sp1;
	DROP TABLE test_nested1;
	SAVEPOINT sp2;
	DROP TABLE test_nested2;
	ROLLBACK TO sp2;
	SAVEPOINT sp3;
	DROP TABLE test_nested3;
	RELEASE SAVEPOINT sp3;
	RELEASE SAVEPOINT sp1;
	COMMIT;
});
is(count_drop_logs(drop_table_pattern('public', 'test_nested1')), 1, 'Test 19: nested1 logged');
is(count_drop_logs(drop_table_pattern('public', 'test_nested2')), 0, 'Test 19: nested2 rolled back');
is(count_drop_logs(drop_table_pattern('public', 'test_nested3')), 1, 'Test 19: nested3 logged');
$node->safe_psql('postgres', 'DROP TABLE IF EXISTS test_nested2;');

# Test 20: Multiple SAVEPOINTs with partial rollbacks
start_new_test('Test 20: Multiple SAVEPOINTs');
$node->safe_psql('postgres', q{
	CREATE TABLE test_msp1 (id int);
	CREATE TABLE test_msp2 (id int);
	CREATE TABLE test_msp3 (id int);
	CREATE TABLE test_msp4 (id int);
	BEGIN;
	SAVEPOINT sp_a;
	DROP TABLE test_msp1;
	SAVEPOINT sp_b;
	DROP TABLE test_msp2;
	SAVEPOINT sp_c;
	DROP TABLE test_msp3;
	ROLLBACK TO sp_b;
	DROP TABLE test_msp4;
	COMMIT;
});
is(count_drop_logs(drop_table_pattern('public', 'test_msp1')), 1, 'Test 20: msp1 logged');
is(count_drop_logs(drop_table_pattern('public', 'test_msp2')), 0, 'Test 20: msp2 rolled back');
is(count_drop_logs(drop_table_pattern('public', 'test_msp3')), 0, 'Test 20: msp3 rolled back');
is(count_drop_logs(drop_table_pattern('public', 'test_msp4')), 1, 'Test 20: msp4 logged');
$node->safe_psql('postgres', 'DROP TABLE IF EXISTS test_msp2, test_msp3;');

# Test 21: Table dropped in failed transaction block
start_new_test('Test 21: Transaction block error suppresses drop log');
$node->safe_psql('postgres', 'CREATE TABLE test_err (id int);');
$node->psql('postgres', q{
	BEGIN;
	DROP TABLE test_err;
	SELECT 1/0;
	COMMIT;
});
is(count_drop_logs(drop_table_pattern('public', 'test_err')), 0, 'Test 21: Transaction block error suppresses drop log');
$node->safe_psql('postgres', 'DROP TABLE IF EXISTS test_err;');

# Test 22: Two-Phase Commit (2PC) DROP warning
start_new_test('Test 22: 2PC DROP emits prepared transaction warning');
$node->safe_psql('postgres', q{
	CREATE TABLE test_2pc_drop (id int);
	BEGIN;
	DROP TABLE test_2pc_drop;
	PREPARE TRANSACTION 'tx_2pc_drop';
	COMMIT PREPARED 'tx_2pc_drop';
});
my @prep_drop_lines = get_log_lines(qr/table "public\.test_2pc_drop" .* dropped inside a prepared transaction/);
is(scalar @prep_drop_lines, 1, 'Test 22: 2PC DROP warning logged');

# Test 23: Two-Phase Commit (2PC) TRUNCATE warning
start_new_test('Test 23: 2PC TRUNCATE emits prepared transaction warning');
$node->safe_psql('postgres', q{
	CREATE TABLE test_2pc_trunc (id int);
	BEGIN;
	TRUNCATE TABLE test_2pc_trunc;
	PREPARE TRANSACTION 'tx_2pc_trunc';
	COMMIT PREPARED 'tx_2pc_trunc';
});
my @prep_trunc_lines = get_log_lines(qr/table "public\.test_2pc_trunc" .* truncated inside a prepared transaction/);
is(scalar @prep_trunc_lines, 1, 'Test 23: 2PC TRUNCATE warning logged');
$node->safe_psql('postgres', 'DROP TABLE test_2pc_trunc;');

# Test 24: DROP DATABASE logged with intermediate LSN
test_drop_logged(
	'Test 24: DROP DATABASE',
	q{
		CREATE DATABASE test_drop_db;
		DROP DATABASE test_drop_db;
	},
	drop_database_pattern('test_drop_db')
);

# Test 25: DROP DATABASE nonexistent with IF EXISTS
test_drop_not_logged(
	'Test 25: DROP DATABASE IF EXISTS nonexistent',
	q{
		DROP DATABASE IF EXISTS non_existent_database_12345;
	},
	drop_database_pattern('non_existent_database_12345')
);

# Test 26: Simple TRUNCATE TABLE
test_drop_logged(
	'Test 26: Simple TRUNCATE TABLE',
	q{
		CREATE TABLE test_trunc_simple (id int);
		INSERT INTO test_trunc_simple VALUES (1);
		TRUNCATE test_trunc_simple;
	},
	truncate_table_pattern('public', 'test_trunc_simple')
);

# Test 27: TRUNCATE TABLE in transaction
test_drop_logged(
	'Test 27: TRUNCATE in transaction',
	q{
		CREATE TABLE test_trunc_xact (id int);
		INSERT INTO test_trunc_xact VALUES (1);
		BEGIN;
		TRUNCATE test_trunc_xact;
		COMMIT;
	},
	truncate_table_pattern('public', 'test_trunc_xact')
);

# Test 28: Rolled back TRUNCATE not logged
test_drop_not_logged(
	'Test 28: Rolled back TRUNCATE',
	q{
		CREATE TABLE test_trunc_rollback (id int);
		INSERT INTO test_trunc_rollback VALUES (1);
		BEGIN;
		TRUNCATE test_trunc_rollback;
		ROLLBACK;
	},
	truncate_table_pattern('public', 'test_trunc_rollback')
);

# Test 29: Multiple tables in single TRUNCATE statement
test_multiple_truncates(
	'Test 29: Multiple tables in single TRUNCATE statement',
	q{
		CREATE TABLE test_trunc_multi1 (id int);
		CREATE TABLE test_trunc_multi2 (id int);
		CREATE TABLE test_trunc_multi3 (id int);
		BEGIN;
		TRUNCATE test_trunc_multi1, test_trunc_multi2, test_trunc_multi3;
		COMMIT;
	},
	['public', 'test_trunc_multi1', 1],
	['public', 'test_trunc_multi2', 1],
	['public', 'test_trunc_multi3', 1]
);

# Test 30: TRUNCATE PARTITIONED TABLE - all tables logged
test_multiple_truncates(
	'Test 30: TRUNCATE Partitioned table',
	q{
		CREATE TABLE test_trunc_part (id int, created_at date) PARTITION BY RANGE (created_at);
		CREATE TABLE test_trunc_part_2024 PARTITION OF test_trunc_part
		    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
		CREATE TABLE test_trunc_part_2025 PARTITION OF test_trunc_part
		    FOR VALUES FROM ('2025-01-01') TO ('2026-01-01');
		BEGIN;
		TRUNCATE test_trunc_part;
		COMMIT;
	},
	['public', 'test_trunc_part', 1],
	['public', 'test_trunc_part_2024', 1],
	['public', 'test_trunc_part_2025', 1]
);

# Test 31: Explicit TRUNCATE of child partition directly is logged
test_drop_logged(
	'Test 31: Direct TRUNCATE of child partition',
	q{
		TRUNCATE TABLE test_trunc_part_2024;
	},
	truncate_table_pattern('public', 'test_trunc_part_2024')
);
$node->safe_psql('postgres', 'DROP TABLE test_trunc_part;');

# Test 32: TRUNCATE CASCADE - cascaded referencing table is logged
test_multiple_truncates(
	'Test 32: TRUNCATE CASCADE with FK references',
	q{
		CREATE TABLE test_trunc_parent (id int PRIMARY KEY);
		CREATE TABLE test_trunc_child (id int, p_id int REFERENCES test_trunc_parent(id));
		BEGIN;
		TRUNCATE test_trunc_parent CASCADE;
		COMMIT;
	},
	['public', 'test_trunc_parent', 1],
	['public', 'test_trunc_child', 1]
);
$node->safe_psql('postgres', 'DROP TABLE test_trunc_child, test_trunc_parent;');

# Test 33: VACUUM FULL and CLUSTER (should NOT produce false DROP TABLE logs)
test_drop_not_logged(
	'Test 33: VACUUM FULL and CLUSTER do not produce false DROP TABLE log',
	q{
		CREATE TABLE test_vac_full (id int);
		CREATE INDEX test_vac_full_idx ON test_vac_full(id);
		INSERT INTO test_vac_full VALUES (1);
		VACUUM FULL test_vac_full;
		CLUSTER test_vac_full USING test_vac_full_idx;
		DROP TABLE test_vac_full;
	},
	qr/table ".*pg_temp.*" \(OID \d+\) dropped/
);

done_testing();
