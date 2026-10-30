# Copyright (c) 2026, PostgreSQL Global Development Group

# Test log_object_drops: committed DROP TABLE and TRUNCATE TABLE are logged
# with the start LSN of the commit record, and point-in-time recovery to that
# LSN brings the data back.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(has_archiving => 1, allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
log_object_drops = on
max_prepared_transactions = 5
});
$node->start;

my $lsn_re = qr/[0-9A-F]+\/[0-9A-F]+/;
my $any_logged_re = qr/\(OID \d+\) (?:dropped|truncated) at LSN /;
my $hint_re = qr/HINT:\s+To recover dropped or truncated tables/;

# Run SQL in a single session and return what it added to the server log.
sub log_for
{
	my ($sql) = @_;
	my $offset = -s $node->logfile;

	$node->safe_psql('postgres', $sql);
	return slurp_file($node->logfile, $offset);
}

sub count_matches
{
	my ($log, $re) = @_;
	my @matches = $log =~ /$re/g;

	return scalar @matches;
}

sub dropped_re
{
	my ($rel) = @_;
	return qr/table "\Q$rel\E" \(OID \d+\) dropped at LSN $lsn_re/;
}

sub truncated_re
{
	my ($rel) = @_;
	return qr/table "\Q$rel\E" \(OID \d+\) truncated at LSN $lsn_re/;
}

# Recover a new node from the base backup up to (but not including) the
# given LSN.  The WAL containing that LSN must already be archived.
sub recover_to
{
	my ($name, $target_lsn) = @_;

	my $pitr = PostgreSQL::Test::Cluster->new($name);
	$pitr->init_from_backup($node, 'bkp', has_restoring => 1, standby => 0);
	$pitr->append_conf(
		'postgresql.conf', qq{
recovery_target_lsn = '$target_lsn'
recovery_target_inclusive = false
recovery_target_action = 'promote'
});
	$pitr->start;
	$pitr->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
	  or die "timed out waiting for $name to be promoted";

	return $pitr;
}

my $log;

# Point-in-time recovery to the logged LSN restores the data.

$node->backup('bkp');

$node->safe_psql(
	'postgres', q{
	CREATE TABLE pitr_drop (id int, val text);
	INSERT INTO pitr_drop VALUES (1, 'alpha'), (2, 'beta');
	CREATE TABLE pitr_trunc (id int, val text);
	INSERT INTO pitr_trunc VALUES (1, 'foo'), (2, 'bar'), (3, 'baz');
});

$log = log_for('DROP TABLE pitr_drop');
my ($drop_lsn) =
  $log =~ /table "public\.pitr_drop" \(OID \d+\) dropped at LSN ($lsn_re)/;
ok(defined $drop_lsn, 'DROP TABLE is logged with an LSN')
  or die "cannot continue without the DROP TABLE LSN";

$log = log_for('TRUNCATE pitr_trunc');
my ($trunc_lsn) =
  $log =~ /table "public\.pitr_trunc" \(OID \d+\) truncated at LSN ($lsn_re)/;
ok(defined $trunc_lsn, 'TRUNCATE is logged with an LSN')
  or die "cannot continue without the TRUNCATE LSN";

my $walfile =
  $node->safe_psql('postgres', 'SELECT pg_walfile_name(pg_current_wal_lsn())');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node->poll_query_until('postgres',
	"SELECT '$walfile' <= last_archived_wal FROM pg_stat_archiver")
  or die "timed out waiting for WAL archiving";

my $pitr = recover_to('pitr_drop', $drop_lsn);
is( $pitr->safe_psql(
		'postgres', "SELECT string_agg(val, ',' ORDER BY id) FROM pitr_drop"),
	'alpha,beta',
	'recovery to the logged LSN restores the dropped table');
$pitr->teardown_node;

$pitr = recover_to('pitr_trunc', $trunc_lsn);
is( $pitr->safe_psql(
		'postgres', "SELECT string_agg(val, ',' ORDER BY id) FROM pitr_trunc"),
	'foo,bar,baz',
	'recovery to the logged LSN restores the truncated rows');
$pitr->teardown_node;

# A committed DROP TABLE logs the LSN and a recovery hint.

$node->safe_psql('postgres', 'CREATE TABLE t_simple (id int)');
$log = log_for('DROP TABLE t_simple');
is(count_matches($log, dropped_re('public.t_simple')),
	1, 'committed DROP TABLE is logged');
is(count_matches($log, $hint_re), 1, 'recovery hint is logged');
unlike($log, qr/at LSN 0\/0+\b/, 'logged LSN is valid');

# Rolled back DROP and TRUNCATE are not logged.

$node->safe_psql('postgres', 'CREATE TABLE t_rollback (id int)');
$log = log_for(
	q{
	BEGIN;
	DROP TABLE t_rollback;
	ROLLBACK;
	BEGIN;
	TRUNCATE t_rollback;
	ROLLBACK;
});
is(count_matches($log, $any_logged_re),
	0, 'rolled back DROP TABLE and TRUNCATE are not logged');
$node->safe_psql('postgres', 'DROP TABLE t_rollback');

# Several tables in one transaction share one LSN and get one hint.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_multi1 (id int);
	CREATE TABLE t_multi2 (id int);
	CREATE TABLE t_multi3 (id int);
});
$log = log_for(
	q{
	BEGIN;
	DROP TABLE t_multi1, t_multi2;
	TRUNCATE t_multi3;
	COMMIT;
});
is(count_matches($log, dropped_re('public.t_multi1')),
	1, 'first table of a multi-table DROP is logged');
is(count_matches($log, dropped_re('public.t_multi2')),
	1, 'second table of a multi-table DROP is logged');
is(count_matches($log, truncated_re('public.t_multi3')),
	1, 'TRUNCATE in the same transaction is logged');
my %lsns = map { $_ => 1 } $log =~ /$any_logged_re($lsn_re)/g;
is(scalar keys %lsns, 1, 'all tables in one transaction share the same LSN');
is(count_matches($log, $hint_re), 1, 'one recovery hint per transaction');
$node->safe_psql('postgres', 'DROP TABLE t_multi3');

# DROP SCHEMA CASCADE logs every table in the schema, including a partition
# whose parent lives in another schema, but not that parent.

$log = log_for(
	q{
	CREATE SCHEMA s_parent;
	CREATE SCHEMA s_drop;
	CREATE TABLE s_drop.t1 (id int);
	CREATE TABLE s_parent.p (id int) PARTITION BY RANGE (id);
	CREATE TABLE s_drop.p1 PARTITION OF s_parent.p FOR VALUES FROM (1) TO (10);
	DROP SCHEMA s_drop CASCADE;
});
is(count_matches($log, dropped_re('s_drop.t1')),
	1, 'DROP SCHEMA CASCADE logs a table in the schema');
is(count_matches($log, dropped_re('s_drop.p1')),
	1, 'DROP SCHEMA CASCADE logs a partition in the schema');
is(count_matches($log, dropped_re('s_parent.p')),
	0, 'DROP SCHEMA CASCADE does not log the parent in another schema');
$node->safe_psql('postgres', 'DROP SCHEMA s_parent CASCADE');

# Partitioned tables: the parent and every partition are logged.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_part (id int) PARTITION BY RANGE (id);
	CREATE TABLE t_part1 PARTITION OF t_part FOR VALUES FROM (1) TO (10);
	CREATE TABLE t_part2 PARTITION OF t_part FOR VALUES FROM (10) TO (20);
});
$log = log_for('TRUNCATE t_part');
is(count_matches($log, truncated_re("public.$_")),
	1, "TRUNCATE of a partitioned table logs $_")
  foreach qw(t_part t_part1 t_part2);
$log = log_for('DROP TABLE t_part');
is(count_matches($log, dropped_re("public.$_")),
	1, "DROP of a partitioned table logs $_")
  foreach qw(t_part t_part1 t_part2);

# Foreign keys: TRUNCATE CASCADE truncates and logs the referencing table,
# while DROP TABLE CASCADE only drops the constraint, so only the dropped
# table is logged.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_fk_parent (id int PRIMARY KEY);
	CREATE TABLE t_fk_child (parent_id int REFERENCES t_fk_parent);
});
$log = log_for('TRUNCATE t_fk_parent CASCADE');
is(count_matches($log, truncated_re('public.t_fk_child')),
	1, 'TRUNCATE CASCADE logs the referencing table');
$log = log_for('DROP TABLE t_fk_parent CASCADE');
is(count_matches($log, dropped_re('public.t_fk_parent')),
	1, 'DROP TABLE CASCADE logs the dropped table');
is(count_matches($log, dropped_re('public.t_fk_child')),
	0, 'DROP TABLE CASCADE does not log a table that only lost a constraint');
$node->safe_psql('postgres', 'DROP TABLE t_fk_child');

# Savepoints: work inside a rolled back savepoint is not logged, including
# work from a nested savepoint that was released into it first.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE sp_a (id int);
	CREATE TABLE sp_b (id int);
	CREATE TABLE sp_c (id int);
	CREATE TABLE sp_d (id int);
	CREATE TABLE sp_e (id int);
});
$log = log_for(
	q{
	BEGIN;
	DROP TABLE sp_a;
	SAVEPOINT s1;
	DROP TABLE sp_b;
	SAVEPOINT s2;
	DROP TABLE sp_c;
	RELEASE SAVEPOINT s2;
	ROLLBACK TO SAVEPOINT s1;
	DROP TABLE sp_d;
	SAVEPOINT s3;
	TRUNCATE sp_e;
	RELEASE SAVEPOINT s3;
	COMMIT;
});
is(count_matches($log, dropped_re('public.sp_a')),
	1, 'drop before any savepoint is logged');
is(count_matches($log, dropped_re('public.sp_b')),
	0, 'drop in a rolled back savepoint is not logged');
is(count_matches($log, dropped_re('public.sp_c')),
	0, 'drop released into a rolled back savepoint is not logged');
is(count_matches($log, dropped_re('public.sp_d')),
	1, 'drop after ROLLBACK TO is logged');
is(count_matches($log, truncated_re('public.sp_e')),
	1, 'truncate in a released savepoint is logged');
$node->safe_psql('postgres', 'DROP TABLE sp_b, sp_c, sp_e');

# A PL/pgSQL exception block rolls back its internal subtransaction.

$node->safe_psql('postgres', 'CREATE TABLE t_exception (id int)');
$log = log_for(
	q{
	DO $$
	BEGIN
		DROP TABLE t_exception;
		RAISE EXCEPTION 'undo the drop';
	EXCEPTION WHEN OTHERS THEN
		NULL;
	END
	$$;
});
is(count_matches($log, dropped_re('public.t_exception')),
	0, 'drop undone by an exception block is not logged');
$node->safe_psql('postgres', 'DROP TABLE t_exception');

# Temporary and unlogged tables, and relations that are not tables, are not
# logged.  The pattern matches any schema, so this also covers the pg_temp_N
# schema of the temporary table.

$log = log_for(
	q{
	CREATE TEMP TABLE t_temp (id int);
	TRUNCATE t_temp;
	DROP TABLE t_temp;
	CREATE UNLOGGED TABLE t_unlogged (id int);
	TRUNCATE t_unlogged;
	DROP TABLE t_unlogged;
	CREATE TABLE t_base (id int);
	CREATE VIEW t_view AS SELECT * FROM t_base;
	CREATE MATERIALIZED VIEW t_matview AS SELECT * FROM t_base;
	CREATE INDEX t_base_idx ON t_base (id);
	CREATE SEQUENCE t_seq;
	DROP VIEW t_view;
	DROP MATERIALIZED VIEW t_matview;
	DROP INDEX t_base_idx;
	DROP SEQUENCE t_seq;
	DROP TABLE IF EXISTS t_does_not_exist;
});
is(count_matches($log, $any_logged_re),
	0, 'temporary, unlogged and non-table relations are not logged');
$node->safe_psql('postgres', 'DROP TABLE t_base');

# Commands that rewrite a table drop a transient heap internally, which must
# not be logged.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_rewrite (id int);
	CREATE INDEX t_rewrite_idx ON t_rewrite (id);
	INSERT INTO t_rewrite VALUES (1);
	CREATE MATERIALIZED VIEW t_rewrite_mv AS SELECT 1 AS a;
});
$log = log_for(
	q{
	ALTER TABLE t_rewrite ALTER COLUMN id TYPE bigint;
	VACUUM FULL t_rewrite;
	CLUSTER t_rewrite USING t_rewrite_idx;
	REFRESH MATERIALIZED VIEW t_rewrite_mv;
});
is(count_matches($log, $any_logged_re),
	0, 'table rewrites do not log a drop');
$node->safe_psql('postgres',
	'DROP TABLE t_rewrite; DROP MATERIALIZED VIEW t_rewrite_mv;');

# Nothing is logged when the parameter is off.

$node->safe_psql('postgres', 'CREATE TABLE t_off (id int)');
$log = log_for(
	q{
	SET log_object_drops = off;
	TRUNCATE t_off;
	DROP TABLE t_off;
});
is(count_matches($log, $any_logged_re),
	0, 'nothing is logged with log_object_drops = off');

# Nothing is logged for prepared transactions.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_2pc_drop (id int);
	CREATE TABLE t_2pc_trunc (id int);
});
$log = log_for(
	q{
	BEGIN;
	DROP TABLE t_2pc_drop;
	TRUNCATE t_2pc_trunc;
	PREPARE TRANSACTION 'p1';
	COMMIT PREPARED 'p1';
});
is(count_matches($log, $any_logged_re),
	0, 'no recovery LSN is logged for a prepared transaction');
$node->safe_psql('postgres', 'DROP TABLE t_2pc_trunc');

# The hint is still logged once when the last registered drop is rolled back.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE hint_a (id int);
	CREATE TABLE hint_b (id int);
});
$log = log_for(
	q{
	BEGIN;
	DROP TABLE hint_a;
	SAVEPOINT s;
	DROP TABLE hint_b;
	ROLLBACK TO SAVEPOINT s;
	COMMIT;
});
is(count_matches($log, dropped_re('public.hint_a')),
	1, 'drop before the rolled back savepoint is logged');
is(count_matches($log, $hint_re),
	1, 'hint is logged once when the last drop was rolled back');
$node->safe_psql('postgres', 'DROP TABLE hint_b');

# A rolled back drop does not leak into the next transaction.

$node->safe_psql(
	'postgres', q{
	CREATE TABLE leak_a (id int);
	CREATE TABLE leak_b (id int);
});
$log = log_for(
	q{
	BEGIN;
	DROP TABLE leak_a;
	ROLLBACK;
	DROP TABLE leak_b;
});
is(count_matches($log, dropped_re('public.leak_a')),
	0, 'rolled back drop is not logged by the next transaction');
is(count_matches($log, dropped_re('public.leak_b')),
	1, 'drop in the next transaction is logged');
$node->safe_psql('postgres', 'DROP TABLE leak_a');

done_testing();
