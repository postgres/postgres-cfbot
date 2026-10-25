# Copyright (c) 2026, PostgreSQL Global Development Group

# Test concurrent index builds with inserts, updates, and vacuuming.
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
	$pgbench_clients, $pgbench_jobs, $pgbench_transactions);
my $no_hot = $is_stress ? int(rand(2)) : 0;
note("no_hot = $no_hot");

note(
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

#
# Test set-up
#
$node = PostgreSQL::Test::Cluster->new('RC_test');
$node->init;
$node->append_conf('postgresql.conf',
	'lock_timeout = ' . (1000 * $PostgreSQL::Test::Utils::timeout_default));
$node->append_conf('postgresql.conf', 'fsync = off');
$node->append_conf('postgresql.conf', 'maintenance_work_mem = 32MB')
  ;    # to avoid OOM
$node->append_conf('postgresql.conf', 'shared_buffers = 32MB'); # to avoid OOM
# predicate_stable() calls txid_current(), which cannot run in a
# parallel worker.  Disable forced parallel queries for this test.
$node->append_conf('postgresql.conf', 'debug_parallel_query = off');
# Run autovacuum often enough to remove dead heap and TOAST tuples
# while index builds are in progress.
$node->append_conf('postgresql.conf', 'autovacuum_naptime = 1s');
# Reset the snapshot of a concurrent build at every page boundary.
$node->append_conf('postgresql.conf',
	'concurrent_index_reset_snapshot_interval = 0');
$node->start;
$node->safe_psql('postgres', q(CREATE EXTENSION amcheck));
$node->safe_psql(
	'postgres', q(CREATE UNLOGGED TABLE tbl(i int primary key,
								c1 money default 0, c2 money default 0,
								c3 money default 0, updated_at timestamp,
								ia int4[], p point)));

# Make updates of updated_at non-HOT.  The predicate prevents this
# index from being used by the verification queries.
if ($no_hot)
{
	$node->safe_psql('postgres',
		q(CREATE INDEX CONCURRENTLY idx ON tbl(i, updated_at) WHERE i < 0;));
}

# Create the sequence.
$node->safe_psql(
	'postgres', q(ALTER TABLE tbl SET (autovacuum_vacuum_threshold = 0,
	autovacuum_vacuum_scale_factor = 0, autovacuum_vacuum_cost_delay = 0)));
$node->safe_psql('postgres',
	q(CREATE UNLOGGED SEQUENCE in_row_rebuild START 1 INCREMENT 1;));
$node->safe_psql('postgres', q(SELECT nextval('in_row_rebuild');));

# Create helper functions for predicate tests
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION predicate_stable() RETURNS bool IMMUTABLE
	LANGUAGE plpgsql AS $$
	BEGIN
		EXECUTE 'SELECT txid_current()';
		RETURN true;
	END; $$;
));

$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION predicate_const(integer) RETURNS bool IMMUTABLE PARALLEL SAFE
	LANGUAGE plpgsql AS $$
	BEGIN
		RETURN MOD($1, 2) = 0;
	END; $$;
));

# Exercise retained TOAST values while concurrent updates and vacuuming
# remove old versions.  Use values large enough to require external
# storage even with 32kB pages.
$node->safe_psql('postgres',
	q(CREATE UNLOGGED TABLE toast_tbl(i int primary key, t text)));
$node->safe_psql('postgres',
	q(ALTER TABLE toast_tbl ALTER COLUMN t SET STORAGE EXTERNAL));
$node->safe_psql(
	'postgres', q(ALTER TABLE toast_tbl SET (autovacuum_vacuum_threshold = 0,
	autovacuum_vacuum_scale_factor = 0, autovacuum_vacuum_cost_delay = 0)));
$node->safe_psql(
	'postgres', q(INSERT INTO toast_tbl
								SELECT g, repeat(md5(g::text) || ' ', 300)
								FROM generate_series(1, 2000) g));
is( $node->safe_psql(
		'postgres',
		q(SELECT count(*) > 0 FROM pg_toast.pg_toast_)
		  . $node->safe_psql('postgres', q(SELECT 'toast_tbl'::regclass::oid))
	),
	't',
	'toast_tbl values are stored out of line');

# Exercise a support function that needs an active snapshot while
# sorting, including in parallel workers.
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION cmp_sql(int, int) RETURNS int IMMUTABLE
	LANGUAGE plpgsql AS $$
	BEGIN
		EXECUTE 'SELECT 1';
		RETURN CASE WHEN $1 < $2 THEN -1 WHEN $1 > $2 THEN 1 ELSE 0 END;
	END; $$;
));

$node->safe_psql(
	'postgres', q(
	CREATE OPERATOR CLASS int4_sql_ops FOR TYPE int4 USING btree AS
		OPERATOR 1 <, OPERATOR 2 <=, OPERATOR 3 =, OPERATOR 4 >=, OPERATOR 5 >,
		FUNCTION 1 cmp_sql(int, int);
));

# For indexes whose heap coverage amcheck cannot verify, compare index
# and sequential scans under one snapshot.  Check the plan to ensure
# it uses the index under test.
$node->safe_psql(
	'postgres', q(
	CREATE FUNCTION verify_other(variant int) RETURNS void
	LANGUAGE plpgsql AS $$
	DECLARE
		q text;
		by_index bigint;
		by_seqscan bigint;
		r record;
		uses_index bool := false;
	BEGIN
		CASE variant
			WHEN 0, 3 THEN q := 'SELECT count(*) FROM tbl WHERE p <@ box ''((0,0),(0.5,0.5))''';
			WHEN 1 THEN q := 'SELECT count(*) FROM tbl WHERE updated_at <= now()';
			WHEN 2 THEN q := 'SELECT count(*) FROM tbl WHERE updated_at = (SELECT max(updated_at) FROM tbl)';
			WHEN 4 THEN q := 'SELECT count(*) FROM toast_tbl WHERE t < ''f''';
			WHEN 5 THEN
				PERFORM gin_index_check('new_idx');
				q := 'SELECT count(*) FROM toast_tbl WHERE to_tsvector(''simple''::regconfig, t) @@ to_tsquery(''simple'', ''a:*'')';
			WHEN 6 THEN
				PERFORM gin_index_check('new_idx');
				q := 'SELECT count(*) FROM tbl WHERE ia @> ARRAY[7]';
		END CASE;
		PERFORM set_config('enable_seqscan', 'off', true);
		FOR r IN EXECUTE 'EXPLAIN ' || q LOOP
			uses_index := uses_index OR r."QUERY PLAN" LIKE '%new_idx%';
		END LOOP;
		IF NOT uses_index THEN
			RAISE EXCEPTION 'the plan does not use new_idx';
		END IF;
		EXECUTE q INTO by_index;
		PERFORM set_config('enable_seqscan', 'on', true);
		PERFORM set_config('enable_indexscan', 'off', true);
		PERFORM set_config('enable_indexonlyscan', 'off', true);
		PERFORM set_config('enable_bitmapscan', 'off', true);
		EXECUTE q INTO by_seqscan;
		IF by_index <> by_seqscan THEN
			RAISE EXCEPTION 'index scan found % rows, sequential scan %',
				by_index, by_seqscan;
		END IF;
	END; $$;
));

# Build and rebuild btree indexes while other sessions upsert rows.
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY',
	{
		'concurrent_ops' => sprintf(
			q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set variant random(0, 6)
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
					\elif :variant = 6
						CREATE INDEX CONCURRENTLY new_idx ON tbl(i int4_sql_ops, updated_at);
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
				-- touches no indexed column, so this update is HOT under any build;
				-- the key comes from pgbench, since a volatile expression in the
				-- WHERE clause could not use the index
				\set upd random(0, :num - 1)
				UPDATE tbl SET c1 = c1 + 1::money WHERE i = :upd;
				SELECT setval('in_row_rebuild', 1);
				COMMIT;
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Build and rebuild unique btree indexes during upserts.
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for unique BTREE',
	{
		'concurrent_ops_unique_idx' => sprintf(
			q(
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
				\set upd random(0, :num - 1)
				UPDATE tbl SET c1 = c1 + 1::money WHERE i = :upd;
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Build and rebuild GIN indexes during upserts.
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for GIN',
	{
		'concurrent_ops_gin_idx' => sprintf(
			q(
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
					BEGIN ISOLATION LEVEL REPEATABLE READ;
					SELECT verify_other(6);
					COMMIT;
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					BEGIN ISOLATION LEVEL REPEATABLE READ;
					SELECT verify_other(6);
					COMMIT;
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1, power(10, random(1, 5)))
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				\set upd random(0, :num - 1)
				UPDATE tbl SET c1 = c1 + 1::money WHERE i = :upd;
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->safe_psql('postgres', q(TRUNCATE TABLE tbl;));

# Build and rebuild GiST, BRIN, hash, and SP-GiST indexes during upserts.
$node->pgbench(
	$pgbench_options,
	0,
	[qr{actually processed}],
	[qr{^$}],
	'concurrent operations with REINDEX/CREATE INDEX CONCURRENTLY for GIST/BRIN/HASH/SPGIST',
	{
		'concurrent_ops_other_idx' => sprintf(
			q(
			SELECT pg_try_advisory_lock(42)::integer AS gotlock \gset
			\if :gotlock
				SELECT nextval('in_row_rebuild') AS last_value \gset
				\set parallels random(0, 4)
				\set use_rr random(0, 9)
				\if :last_value < 3
					ALTER TABLE tbl SET (parallel_workers=:parallels);
					ALTER TABLE toast_tbl SET (parallel_workers=:parallels);
					\if :use_rr = 0
						SET default_transaction_isolation = 'repeatable read';
					\endif
					\set variant random(0, 5)
					\if :variant = 0
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING GIST (p);
					\elif :variant = 1
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING BRIN (updated_at);
					\elif :variant = 2
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING HASH (updated_at);
					\elif :variant = 3
						CREATE INDEX CONCURRENTLY new_idx ON tbl USING SPGIST (p);
					\elif :variant = 4
						CREATE INDEX CONCURRENTLY new_idx ON toast_tbl USING BRIN (t);
					\elif :variant = 5
						CREATE INDEX CONCURRENTLY new_idx ON toast_tbl USING GIN (to_tsvector('simple'::regconfig, t));
					\endif
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					BEGIN ISOLATION LEVEL REPEATABLE READ;
					SELECT verify_other(:variant);
					COMMIT;
					REINDEX INDEX CONCURRENTLY new_idx;
					\set sleep_ms random(0, %d)
					\sleep :sleep_ms ms
					BEGIN ISOLATION LEVEL REPEATABLE READ;
					SELECT verify_other(:variant);
					COMMIT;
					DROP INDEX CONCURRENTLY new_idx;
					RESET default_transaction_isolation;
				\endif
				SELECT pg_advisory_unlock(42);
			\else
				\set num random(1, power(10, random(1, 5)))
				INSERT INTO tbl VALUES(floor(random()*:num),0,0,0,now(),ARRAY[floor(random()*100)::int],point(random(),random()))
					ON CONFLICT(i) DO UPDATE SET updated_at = now(), ia = ARRAY[floor(random()*100)::int], p = point(random(),random());
				\set toast_i random(1, 2000)
				UPDATE toast_tbl SET t = repeat(md5(random()::text) || ' ', 300) WHERE i = :toast_i;
				\set upd random(0, :num - 1)
				UPDATE tbl SET c1 = c1 + 1::money WHERE i = :upd;
				SELECT setval('in_row_rebuild', 1);
			\endif
			), $max_sleep_ms, $max_sleep_ms)
	});

$node->stop;
done_testing();
