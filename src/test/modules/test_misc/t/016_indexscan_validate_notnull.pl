# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that indexscan mechanism can speedup ALTER TABLE ADD NOT NULL

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

# Initialize a test cluster
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init();
# Turn message level up to DEBUG1 so that we get the messages we want to see
$node->append_conf('postgresql.conf', 'client_min_messages = DEBUG1');
$node->start;

# Run a SQL command and return psql's stderr (including debug messages)
sub run_sql_command
{
	my $sql = shift;
	my $stderr;

	$node->psql(
		'postgres',
		$sql,
		stderr => \$stderr,
		on_error_die => 1,
		on_error_stop => 1);
	return $stderr;
}

# Verify that run_sql_command output confirms an index scan is used for the NOT NULL check.
sub is_indexscan_veritify_notnull
{
	my $output = shift;
	return index($output, 'DEBUG:  all new not-null constraints on relation') != -1;
}

my $output;

note "test alter table SET NOT NULL using indexscan with partitioned table";

run_sql_command(
    'CREATE TABLE tp_notnull (
		a int, b int, c int, d int, f1 int default 1,
		f2 int default 2, f3 int default 3, f4 int default 4, f5 int default 5) PARTITION BY range(a);
     CREATE TABLE tp_notnull_1 partition of tp_notnull for values from ( 1 ) to (10);
     CREATE TABLE tp_notnull_2 partition of tp_notnull for values from ( 10 ) to (21);
     INSERT INTO tp_notnull(a, b, c, d) SELECT g, g + 10, g + 15, g FROM generate_series(1,19) g;
     CREATE INDEX ON tp_notnull(b);
     CREATE INDEX ON tp_notnull(c);
	 CREATE INDEX ON tp_notnull(d);
     CREATE INDEX ON tp_notnull(f1) INCLUDE (f2);
     CREATE INDEX ON tp_notnull USING hash (f3 int4_ops);
     ');

# Using index scan for NOT NULL verification requires every not-null column must possess a suitable index
$output = run_sql_command(
	'ALTER TABLE tp_notnull ALTER COLUMN a SET NOT NULL, ADD CONSTRAINT c_nn NOT NULL c');
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan to verify not-null constraints on table tp_notnull');

# All columns have index on it, OK
$output = run_sql_command(
	'ALTER TABLE tp_notnull ALTER COLUMN d SET NOT NULL, ALTER COLUMN c SET NOT NULL, ALTER COLUMN b SET NOT NULL;');
ok(is_indexscan_veritify_notnull($output),
	'using indexscan to verify not-null constraints on column b, c, d');
ok( $output =~
	  m/all new not-null constraints on relation "tp_notnull_1" have been validated by using index scan/,
	'all newly added constraints proved by indexscan');
ok( $output =~
	  m/all new not-null constraints on relation "tp_notnull_2" have been validated by using index scan/,
	'all newly added constraints proved by indexscan');

run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN b DROP NOT NULL;');
# Now using index scans on partitions to quickly verify NOT NULL constraints
$output = run_sql_command('ALTER TABLE tp_notnull_1 ALTER COLUMN b SET NOT NULL;');
ok(is_indexscan_veritify_notnull($output), 'index scan verifies NOT NULL for column b');

run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN b DROP NOT NULL;');
# Cannot use indexscan to verify not-null constraint if table rewrite happens
$output = run_sql_command(
	'ALTER TABLE tp_notnull ALTER COLUMN b SET NOT NULL, ALTER COLUMN b SET DATA TYPE bigint;'
);
ok(!is_indexscan_veritify_notnull($output), 'table rewrite on tp_notnull, cannot use indexscan to verify not-null constraints on it');

run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN d DROP NOT NULL');

# Cannot use indexscan because ALTER TABLE VALIDATE CONSTRAINT only hold
# ShareUpdateExclusiveLock lock on the table
run_sql_command('ALTER TABLE tp_notnull ADD CONSTRAINT tp_notnull_d NOT NULL d NOT VALID;');
$output = run_sql_command(
	'ALTER TABLE tp_notnull VALIDATE CONSTRAINT tp_notnull_d;'
);
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan for ALTER TABLE VALIDATE CONSTRAINT command');

run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN d DROP NOT NULL;
				ALTER TABLE tp_notnull ADD CONSTRAINT tp_notnull_d NOT NULL d NOT VALID;');

# OK, ALTER COLUMN SET NOT NULL acquires AccessExclusiveLock
$output = run_sql_command(
	'ALTER TABLE tp_notnull VALIDATE CONSTRAINT tp_notnull_d, ALTER COLUMN b SET NOT NULL;'
);
ok(is_indexscan_veritify_notnull($output), 'using indexscan to verify not-null constraints on column b, c, d');

# Indexscan mechanism to verify NOT NULL constraints cannot apply to index
# INCLUDE column
$output = run_sql_command(
	'ALTER TABLE tp_notnull ADD CONSTRAINT nnf4 NOT NULL f2;'
);
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan to validate not-null constraint on tp_notnull');

# Indexscan mechanism to verify NOT NULL constraints cannot apply to non-Btree index
$output = run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN f3 SET NOT NULL;');
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan to validate not-null constraints on tp_notnull');

run_sql_command('CREATE INDEX ON tp_notnull USING btree (f4 int4_ops, f5 int4_ops)');
# Indexscan mechanism to verify NOT NULL constraints only apply to leading column
$output = run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN f4 SET NOT NULL');
ok(is_indexscan_veritify_notnull($output), 'cannot use indexscan to validate not-null constraints on tp_notnull');

$output = run_sql_command('ALTER TABLE tp_notnull ALTER COLUMN f5 SET NOT NULL');
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan to validate not-null constraints on tp_notnull');

run_sql_command('
	create table t(id int, v int) with (autovacuum_enabled = false, fillfactor = 50);
	insert into t values (1, NULL)'
);
run_sql_command('
	begin;
	select count(*) from t;
	update t set v = 5 where id = 1;
	create index t_v_idx on t (v);
	alter table t alter column v set not null;
	commit;'
);
#  t_v_idx has pg_index.indcheckxmin = true: its underlying tuple belongs to a
#  HOT chain, so this index cannot be trusted to answer "is this column NULL"
ok(!is_indexscan_veritify_notnull($output), 'cannot use indexscan to validate not-null constraints on t');

$node->stop('fast');

done_testing();