# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A FLUX table rewrite while an aborted transaction's revert is still pending.
#
# FLUX updates in place, so an aborted transaction whose rollback was handed
# to an undo apply worker (undo_rollback_overflow_size = 0) or that lost a
# crash leaves its images on the page until the undo is applied.  Readers
# walk past them.  A rewrite must copy exactly the committed state too: the
# undo records name the old relfilenode, so once the rewrite commits nothing
# can apply them any more.  An aborted DELETE's row must not be lost, an
# aborted UPDATE's value must not be copied, an aborted INSERT must not come
# back, and the rewritten table must be indistinguishable from a heap twin
# that ran the same statements, on the primary and on a standby.
#
# Rewrites: VACUUM FULL, CLUSTER (sort and index scan), REPACK, ALTER TABLE
# ... SET TABLESPACE (a file copy), ALTER TABLE ... SET ACCESS METHOD heap,
# ALTER TABLE ... ALTER COLUMN ... TYPE, and REPACK (CONCURRENTLY), whose copy
# reads through the decoding's snapshot while the revert is pending.
#
# The live variant holds the undo apply worker at the injection point
# pbu-undo-worker-before-apply (see 084_perbackend_helping_once.pl).  The
# crash variant keeps the worker from starting at all, as
# 076_flux_deferred_rollback.pl does, so that the revert is still pending
# after the restart.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# name => rewrite statements, with T for the table
my @rewrites = (
	[ vacfull => 'VACUUM FULL T' ],
	[
		cluster_sort =>
		  "SET enable_indexscan = off;\nCLUSTER (VERBOSE) T USING T_pkey;\nRESET enable_indexscan"
	],
	[
		cluster_index =>
		  "SET random_page_cost = 0;\nSET seq_page_cost = 1000;\nCLUSTER (VERBOSE) T USING T_pkey;\nRESET random_page_cost;\nRESET seq_page_cost"
	],
	[ repack => 'REPACK T' ],
	[ repack_conc => 'REPACK (CONCURRENTLY) T' ],
	# Copies the file as it is (relation_copy_data) to a new relfilenumber.
	[ set_tablespace => 'ALTER TABLE T SET TABLESPACE ts' ],
	[ set_am_heap => 'ALTER TABLE T SET ACCESS METHOD heap' ],
	[ alter_type => 'ALTER TABLE T ALTER COLUMN v TYPE bigint' ],
	# The rewriting transaction reads its own new rows before it commits.
	[
		alter_type_xact =>
		  "BEGIN;\nALTER TABLE T ALTER COLUMN v TYPE bigint;\nUPDATE T SET pad = pad || '+';\nCOMMIT"
	],);

# Run $sql once per table, T replaced by the table name.
sub each_tab
{
	my ($sql, @tabs) = @_;
	return join("\n", map { (my $x = $sql) =~ s/\bT(?=\b|_)/$_/g; $x } @tabs);
}

# Row 3's aborted value, 16908801 = 0x01020201, reads the same in either byte
# order, so the raw page can be searched for it.
sub on_page
{
	my $t = shift;
	return
	  "SELECT position('\\x01020201'::bytea IN get_raw_page('$t', 0)) > 0";
}

# Content of a table as count and md5, through a seqscan and through the
# primary key.
sub content
{
	my ($node, $t) = @_;
	my $q =
	  "SELECT count(*) || ':' || md5(coalesce(string_agg(x::text, ',' ORDER BY id), '')) FROM $t x";
	return $node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; $q;\n"
		  . "SET enable_seqscan = off; SET enable_indexscan = on; $q;");
}

# bt_index_check (heapallindexed) of one relation.  bt_index_check returns
# void and reports corruption with an ERROR, so it passes when psql succeeds.
# Returns psql's stderr, empty on success.
sub bt_check
{
	my ($node, $rel) = @_;
	my ($out, $err);
	my $rc = $node->psql(
		'postgres',
		"SELECT bt_index_check('$rel'::regclass, heapallindexed => true)",
		stdout => \$out,
		stderr => \$err,
		on_error_stop => 1);
	return $rc == 0 && $err eq '' ? '' : "rc=$rc: $err";
}

# Every row's key is taken: a unique check (dirty snapshot) must see it.
sub dup_count
{
	my ($node, $t) = @_;
	return $node->safe_psql('postgres',
		"WITH i AS (INSERT INTO $t SELECT * FROM $t ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM i"
	);
}

# Check the rewritten FLUX table against its heap twin.
sub check_twin
{
	my ($node, $f, $h, $what) = @_;
	is(content($node, $f), content($node, $h), "$f: same rows as $h, $what");
	is(dup_count($node, $f), '0', "$f: unique checks see every row, $what");
	my @idx = split /\n/,
	  $node->safe_psql('postgres',
		"SELECT c.relname FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid = '$f'::regclass ORDER BY 1"
	  );
	is(join(',', @idx), "${f}_k_idx,${f}_pkey", "$f: both indexes, $what");
	is(bt_check($node, $_), '', "$_: bt_index_check, $what") foreach @idx;
	is( $node->safe_psql(
			'postgres', "SELECT count(*) FROM verify_heapam('$h')"),
		'0',
		"$h: verify_heapam, $what");
}

# The statements every table pair runs.  First committed history: a
# committed DELETE and UPDATE, and a committed transaction with an
# UPDATE, DELETE and INSERT in a savepoint that is rolled back.
my $setup = q(
CREATE TABLE T (id int PRIMARY KEY, v int, k int, pad text) USING AM;
CREATE INDEX T_k_idx ON T (k);
INSERT INTO T SELECT g, g, g, 'p' || g FROM generate_series(1, 30) g;
DELETE FROM T WHERE id = 13;
UPDATE T SET v = v + 1 WHERE id = 14;
);
my $committed_sp = q(
BEGIN;
UPDATE T SET v = v + 1000 WHERE id = 10;
SAVEPOINT s;
UPDATE T SET v = 16974593 WHERE id = 11;
DELETE FROM T WHERE id = 12;
INSERT INTO T VALUES (104, 104, 104, 'sp');
ROLLBACK TO s;
COMMIT;
);

# The aborted transaction: DELETE; in-place UPDATE (same size, larger, of an
# indexed column, twice); a primary key UPDATE (new TID); INSERT; an INSERT
# that is then updated and deleted; and a savepoint with an UPDATE, DELETE
# and INSERT, rolled back to and released.
my $aborted = q(
DELETE FROM T WHERE id IN (1, 2);
UPDATE T SET v = 16908801 WHERE id = 3;
UPDATE T SET v = v + 100, pad = repeat('w', 60) WHERE id = 4;
UPDATE T SET k = k + 1000 WHERE id = 5;
UPDATE T SET id = 1006 WHERE id = 6;
UPDATE T SET v = 71 WHERE id = 7;
UPDATE T SET v = 72 WHERE id = 7;
INSERT INTO T VALUES (101, 101, 101, 'ins');
INSERT INTO T VALUES (102, 102, 102, 'ins');
UPDATE T SET v = 1020 WHERE id = 102;
DELETE FROM T WHERE id = 102;
SAVEPOINT s;
UPDATE T SET v = 999 WHERE id = 8;
DELETE FROM T WHERE id = 9;
INSERT INTO T VALUES (103, 103, 103, 'sp');
ROLLBACK TO s;
RELEASE s;
UPDATE T SET v = v + 1 WHERE id = 15;
UPDATE T SET v = v + 1 WHERE id = 19;
DELETE FROM T WHERE id = 19;
);

# After the rewrite, more DML on the rewritten table: nothing the stale undo
# or sLog state of the old file holds may interfere with it.
my $after = q(
UPDATE T SET v = v + 1;
DELETE FROM T WHERE id % 5 = 0;
INSERT INTO T SELECT g, g, g, 'a' FROM generate_series(201, 210) g;
UPDATE T SET k = k + 1 WHERE id IN (3, 4, 201);
);

sub tabs_of
{
	my ($prefix) = @_;
	return (map { ("${prefix}f_$_->[0]", "${prefix}h_$_->[0]") } @rewrites);
}

sub create_tables
{
	my ($node, $prefix) = @_;
	foreach my $rw (@rewrites)
	{
		foreach my $am ('flux', 'heap')
		{
			my $t = $prefix . substr($am, 0, 1) . "_$rw->[0]";
			(my $s = $setup) =~ s/\bAM\b/$am/;
			$node->safe_psql('postgres', each_tab($s . $committed_sp, $t));
		}
	}
}

sub run_rewrites
{
	my ($node, $prefix) = @_;
	foreach my $rw (@rewrites)
	{
		foreach my $am ('f', 'h')
		{
			my $t = "$prefix${am}_$rw->[0]";
			my ($out, $err);
			my $rc = $node->psql(
				'postgres',
				"SET client_min_messages = info;\n" . each_tab($rw->[1], $t),
				stdout => \$out,
				stderr => \$err,
				on_error_stop => 1);
			is($rc, 0, "$t: $rw->[0] ran") or diag($err);
			like(
				$err,
				qr/"public\.$t" using sequential scan and sort/,
				"$t: CLUSTER sorts") if $rw->[0] eq 'cluster_sort';
			like(
				$err,
				qr/"public\.$t" using index scan/,
				"$t: CLUSTER reads through the index"
			) if $rw->[0] eq 'cluster_index';
		}
	}
}

sub check_all
{
	my ($node, $prefix, $what) = @_;
	# Control: bt_check must be able to fail.
	like(
		bt_check($node, "${prefix}h_vacfull"),
		qr/ERROR:  "${prefix}h_vacfull" is not an index/,
		"control: bt_index_check of a table fails, $what");
	foreach my $rw (@rewrites)
	{
		check_twin($node, "${prefix}f_$rw->[0]", "${prefix}h_$rw->[0]",
			$what);
	}
}

# Is any undo, or any ATM entry, still holding WAL back?
my $retained = q(
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT bool_and(discard = insert) FROM pg_stat_get_undo_logs()
         WHERE persistence = 'permanent'));

###########################################################################
# Live: the undo apply worker is held while the rewrites run.  A standby
# replays all of it.
###########################################################################
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
allow_in_place_tablespaces = on
));
$primary->start;
if (   !$primary->check_extension('injection_points')
	|| !$primary->check_extension('pageinspect')
	|| !$primary->check_extension('amcheck'))
{
	plan skip_all => 'injection_points, pageinspect or amcheck not installed';
}
$primary->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE TABLESPACE ts LOCATION '';
-- Wake one held undo apply worker; true once none is held any more.
CREATE FUNCTION release_one() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
                 WHERE wait_event = 'pbu-undo-worker-before-apply') THEN
    RETURN true;
  END IF;
  BEGIN
    PERFORM injection_points_wakeup('pbu-undo-worker-before-apply');
  EXCEPTION WHEN OTHERS THEN
    NULL;  -- it left the injection point meanwhile
  END;
  RETURN false;
END $$;
));
$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;

create_tables($primary, '');
my @tabs = tabs_of('');
my @ftabs = grep { /^f_/ } @tabs;

$primary->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
$primary->safe_psql('postgres',
	"BEGIN;\n" . each_tab($aborted, @tabs) . "\nROLLBACK;");
$primary->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');

foreach my $t (@ftabs)
{
	is($primary->safe_psql('postgres', on_page($t)),
		't', "$t: the rollback is pending");
	(my $h = $t) =~ s/^f_/h_/;
	is( content($primary, $t),
		content($primary, $h),
		"$t: readers see the committed rows before the rewrite");
}

run_rewrites($primary, '');
check_all($primary, '', 'rewritten while the revert was pending');
foreach my $t (@ftabs)
{
	is($primary->safe_psql('postgres', on_page($t)),
		'f', "$t: the aborted value was not copied");
}
$primary->wait_for_replay_catchup($standby);
foreach my $t (@tabs)
{
	is( content($standby, $t),
		content($primary, $t),
		"$t: standby agrees after the rewrite");
}

# Let the held workers go: they find the records' file gone.
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
ok($primary->poll_query_until('postgres', 'SELECT release_one()'),
	'held undo apply workers released');
ok( $primary->poll_query_until('postgres', $retained),
	'no undo and no ATM entry left holding WAL');
check_all($primary, '', 'after the undo apply worker');

$primary->safe_psql('postgres', each_tab($after, @tabs));
check_all($primary, '', 'after more DML');

$primary->wait_for_replay_catchup($standby);
foreach my $t (@tabs)
{
	is( content($standby, $t),
		content($primary, $t),
		"$t: standby agrees with the primary");
}

$standby->stop;
$primary->stop;

###########################################################################
# Crash: the revert is still pending after the restart, while no undo
# apply worker can start (see 076_flux_deferred_rollback.pl).
###########################################################################
# REPACK (CONCURRENTLY) needs wal_level replica and a background worker for
# its decoding, neither of which this node has; the live variant covers it.
@rewrites = grep { $_->[0] ne 'repack_conc' } @rewrites;
my $node = PostgreSQL::Test::Cluster->new('crash');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
max_logical_replication_workers = 0
max_logical_revert_workers = 0
max_parallel_workers = 0
max_parallel_workers_per_gather = 0
max_parallel_maintenance_workers = 0
# discard worker + UNDO worker + undo worker launcher: no room for an
# undo apply worker
max_worker_processes = 3
allow_in_place_tablespaces = on
));
$node->start;
unlike(
	slurp_file($node->logfile),
	qr/too many background workers/,
	'every static background worker registered');
$node->safe_psql('postgres',
	"CREATE EXTENSION pageinspect; CREATE EXTENSION amcheck; CREATE TABLESPACE ts LOCATION '';"
);
create_tables($node, 'c');
@tabs = tabs_of('c');
@ftabs = grep { /^cf_/ } @tabs;

# One aborted transaction and one that the crash kills.
$node->safe_psql('postgres',
	"BEGIN;\n" . each_tab($aborted, @tabs) . "\nROLLBACK;");
my $bg = $node->background_psql('postgres');
$bg->query_safe(
	"BEGIN;\n"
	  . each_tab(
		"UPDATE T SET v = v + 500 WHERE id IN (16, 17);\nDELETE FROM T WHERE id = 18;\nINSERT INTO T VALUES (105, 105, 105, 'loser');",
		@tabs));
$node->safe_psql('postgres', 'CHECKPOINT');
$node->stop('immediate');
$bg->{run}->kill_kill;
$node->start;

is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'undo apply worker'"
	),
	'0',
	'no undo apply worker ran');
foreach my $t (@ftabs)
{
	is($node->safe_psql('postgres', on_page($t)),
		't', "$t: the rollback is pending after the crash");
}

run_rewrites($node, 'c');
check_all($node, 'c', 'rewritten after the crash, revert pending');

# Now let the undo apply worker run.
$node->append_conf('postgresql.conf', 'max_worker_processes = 8');
$node->restart;
ok( $node->poll_query_until('postgres', $retained),
	'crash: no undo and no ATM entry left holding WAL');
check_all($node, 'c', 'crash, after the undo apply worker');
$node->safe_psql('postgres', each_tab($after, @tabs));
check_all($node, 'c', 'crash, after more DML');

foreach my $n ($primary, $standby, $node)
{
	unlike(
		slurp_file($n->logfile),
		qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
		$n->name . ': no failed assertion, mismatched or abandoned undo');
}
$node->stop;
done_testing();
