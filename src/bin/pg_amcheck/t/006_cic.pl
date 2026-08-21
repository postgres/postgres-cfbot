# Copyright (c) 2026, PostgreSQL Global Development Group

# Test REINDEX CONCURRENTLY with concurrent modifications and HOT updates
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use constant STRESS_PGBENCH_CLIENTS => 30;
use constant STRESS_PGBENCH_JOBS => 8;
use constant STRESS_PGBENCH_TRANSACTIONS => 10000;
use constant STRESS_MAX_SLEEP_MS => 10;

use constant DEFAULT_PGBENCH_CLIENTS => 15;
use constant DEFAULT_PGBENCH_JOBS => 4;
use constant DEFAULT_PGBENCH_TRANSACTIONS => 500;
use constant DEFAULT_MAX_SLEEP_MS => 1;

Test::More->builder->todo_start('filesystem bug')
  if PostgreSQL::Test::Utils::has_wal_read_bug;

my $node;
my $pg_test_extra = $ENV{PG_TEST_EXTRA} // '';
my $is_stress = $pg_test_extra =~ /\bstress\b/ ? 1 : 0;
my $pgbench_clients =
  $is_stress ? STRESS_PGBENCH_CLIENTS : DEFAULT_PGBENCH_CLIENTS;
my $pgbench_jobs = $is_stress ? STRESS_PGBENCH_JOBS : DEFAULT_PGBENCH_JOBS;
my $pgbench_transactions =
  $is_stress ? STRESS_PGBENCH_TRANSACTIONS : DEFAULT_PGBENCH_TRANSACTIONS;
my $max_sleep_ms = $is_stress ? STRESS_MAX_SLEEP_MS : DEFAULT_MAX_SLEEP_MS;
my $pgbench_options = sprintf(
	'--no-vacuum --client=%d --jobs=%d --exit-on-abort --transactions=%d',
	$pgbench_clients,
	$pgbench_jobs,
	$pgbench_transactions);
my $no_hot = $is_stress ? int(rand(2)) : 0;

print(
		sprintf(
		'settings: PG_TEST_EXTRA=%s stress=%d clients=%d jobs=%d transactions=%d max_sleep_ms=%d no_hot=%d',
		defined($ENV{PG_TEST_EXTRA})
		? ($pg_test_extra eq '' ? '(empty)' : $pg_test_extra)
		: '(undef)',
		$is_stress,
		$pgbench_clients,
		$pgbench_jobs,
		$pgbench_transactions,
		$max_sleep_ms,
		$no_hot));
print "\n";

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('RC_test');
$node->init;
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $PostgreSQL::Test::Utils::timeout_default));
$node->append_conf('postgresql.conf', 'fsync = off');
$node->append_conf('postgresql.conf', 'maintenance_work_mem = 32MB'); # to avoid OOM
$node->append_conf('postgresql.conf', 'shared_buffers = 32MB'); # to avoid OOM
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql('postgres', q(CREATE UNLOGGED TABLE tbl(i int primary key,
								c1 money default 0, c2 money default 0,
								c3 money default 0, updated_at timestamp,
								ia int4[], p point)));

if ($no_hot) { $node->safe_psql('postgres', q(CREATE INDEX CONCURRENTLY idx ON tbl(i, updated_at);)); }

# create sequence
$node->safe_psql('postgres', q(CREATE UNLOGGED SEQUENCE in_row_rebuild START 1 INCREMENT 1;));
$node->safe_psql('postgres', q(SELECT nextval('in_row_rebuild');));

# Create helper functions for predicate tests
$node->safe_psql('postgres', q(
	CREATE FUNCTION predicate_stable() RETURNS bool IMMUTABLE
	LANGUAGE plpgsql AS $$
	BEGIN
		EXECUTE 'SELECT txid_current()';
		RETURN true;
	END; $$;
));

$node->safe_psql('postgres', q(
	CREATE FUNCTION predicate_const(integer) RETURNS bool IMMUTABLE
	LANGUAGE plpgsql AS $$
	BEGIN
		RETURN MOD($1, 2) = 0;
	END; $$;
));

# Run CIC/RIC in different options concurrently with upserts
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY',
	{
		'concurrent_ops' => sprintf(q(
			SET debug_parallel_query = off; -- this is because predicate_stable implementation
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set variant random(0, 5)
				\set parallels random(0, 4)
				\set use_rr random(0, 9)
				\if :last_value < 3
					ALTER TABLE tbl SET (parallel_workers=:parallels);
					\if :use_rr = 0
						SET default_transaction_isolation = 'repeatable read';
					\endif
					\if :variant = 0
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i, updated_at);
					\elif :variant = 1
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i, updated_at) WHERE predicate_stable();
					\elif :variant = 2
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i, updated_at) WHERE MOD(i, 2) = 0;
					\elif :variant = 3
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i, updated_at) WHERE predicate_const(i);
					\elif :variant = 4
						CREATE INDEX CONCURRENTLY new_idx ON tbl(predicate_const(i));
					\elif :variant = 5
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i, predicate_const(i), updated_at) WHERE predicate_const(i);
					\endif
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT bt_index_check('new_idx', heapallindexed => true, checkunique => true);
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT bt_index_check('new_idx', heapallindexed => true, checkunique => true);
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1000, 100000)
				BEGIN;
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				SELECT setval('in_row_rebuild', 1);
				COMMIT;
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Run CIC/RIC for unique index concurrently with upserts
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for unique BTREE',
	{
		'concurrent_ops_unique_idx' => sprintf(q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set parallels random(0, 4)
				\set use_rr random(0, 9)
				\if :last_value < 3
					ALTER TABLE tbl SET (parallel_workers=:parallels);
					\if :use_rr = 0
						SET default_transaction_isolation = 'repeatable read';
					\endif
					CREATE UNIQUE INDEX CONCURRENTLY new_idx ON tbl(i);
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT bt_index_check('new_idx', heapallindexed => true, checkunique => true);
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT bt_index_check('new_idx', heapallindexed => true, checkunique => true);
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1, power(10, random(1, 5)))
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Run CIC/RIC for GIN with upserts
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for GIN',
	{
		'concurrent_ops_gin_idx' => sprintf(q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set parallels random(0, 4)
				\set use_rr random(0, 9)
				\if :last_value < 3
					ALTER TABLE tbl SET (parallel_workers=:parallels);
					\if :use_rr = 0
						SET default_transaction_isolation = 'repeatable read';
					\endif
					CREATE INDEX CONCURRENTLY new_idx ON tbl USING GIN (ia);
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT gin_index_check('new_idx');
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					SELECT gin_index_check('new_idx');
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1, power(10, random(1, 5)))
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Run CIC/RIC for GIST/BRIN/HASH/SPGIST index concurrently with upserts
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for GIST/BRIN/HASH/SPGIST',
	{
		'concurrent_ops_other_idx' => sprintf(q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set parallels random(0, 4)
				\set use_rr random(0, 9)
				\if :last_value < 3
					ALTER TABLE tbl SET (parallel_workers=:parallels);
					\if :use_rr = 0
						SET default_transaction_isolation = 'repeatable read';
					\endif
					\set variant random(0, 3)
					\if :variant = 0
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING GIST (p);
					\elif :variant = 1
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING BRIN (updated_at);
					\elif :variant = 2
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING HASH (updated_at);
					\elif :variant = 3
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING SPGIST (p);
					\endif
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1, power(10, random(1, 5)))
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
		});

$node->stop;
done_testing();
