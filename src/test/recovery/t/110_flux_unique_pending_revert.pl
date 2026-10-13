# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX uniqueness and exclusion checks while an aborted transaction's revert
# is still pending.
#
# FLUX updates in place, so an aborted transaction whose rollback was handed
# to an undo apply worker (undo_rollback_overflow_size = 0) leaves its images
# on the page until the undo is applied; an in-place UPDATE also re-stamps
# t_xmin to the updater.  A uniqueness check (SnapshotDirty, SnapshotSelf)
# must still see the row at its committed before-image: a PRIMARY KEY, a
# UNIQUE constraint (immediate and deferred), an exclusion constraint (gist,
# hash) and ON CONFLICT must all behave as on a heap twin that ran the same
# statements.  The same with an inline rollback, with the conflicting writer
# still in progress (the check must wait for it), after the undo apply
# worker has applied the rollback, and on a standby.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# The aborted value, 16908801 = 0x01020201, reads the same in either byte
# order, so the raw page can be searched for it.
my $X = 16908801;

# name, DDL (T = the table), aborted statements, probe, expected probe result
# ("ok:" and the rows it returned, or the SQLSTATE of its error).
my @cases = (
	[
		upd_nonkey => 'id int PRIMARY KEY, v int',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		upd_key_old => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		upd_key_new => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		"INSERT INTO T VALUES ($X, 333) RETURNING *", "ok:$X|333"
	],
	[
		uq_inplace_old => 'id int, v int UNIQUE',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (33, 3)', '23505'
	],
	[
		uq_inplace_new => 'id int, v int UNIQUE',
		"UPDATE T SET v = $X WHERE id = 3",
		"INSERT INTO T VALUES (33, $X) RETURNING *", "ok:33|$X"
	],
	[
		del_then_ins => 'id int PRIMARY KEY, v int',
		"DELETE FROM T WHERE id = 3; INSERT INTO T VALUES (3, $X)",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		del => 'id int PRIMARY KEY, v int',
		'DELETE FROM T WHERE id = 3',
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		oc_nothing => 'id int PRIMARY KEY, v int',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333) ON CONFLICT DO NOTHING RETURNING *',
		'ok:'
	],
	[
		oc_update => 'id int PRIMARY KEY, v int',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333) ON CONFLICT (id) DO UPDATE SET v = T.v + 1000 RETURNING *',
		'ok:3|1003'
	],
	[
		oc_update_key_old => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333) ON CONFLICT (id) DO UPDATE SET v = T.v + 1000 RETURNING *',
		'ok:3|1003'
	],
	[
		oc_update_key_new => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		"INSERT INTO T VALUES ($X, 333) ON CONFLICT (id) DO UPDATE SET v = T.v + 1000 RETURNING *",
		"ok:$X|333"
	],
	[
		oc_nothing_key_old => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333) ON CONFLICT DO NOTHING RETURNING *',
		'ok:'
	],
	[
		oc_nothing_key_new => 'id int PRIMARY KEY, v int',
		"UPDATE T SET id = $X WHERE id = 3",
		"INSERT INTO T VALUES ($X, 333) ON CONFLICT DO NOTHING RETURNING *",
		"ok:$X|333"
	],
	[
		oc_update_uq_old => 'id int PRIMARY KEY, v int UNIQUE',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (33, 3) ON CONFLICT (v) DO UPDATE SET id = T.id + 1000 RETURNING *',
		'ok:1003|3'
	],
	# An in-place UPDATE of another indexed column: the primary key entry
	# is live, the row's image says "keys updated".
	[
		sec_index => 'id int PRIMARY KEY, v int, k int UNIQUE',
		"UPDATE T SET k = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	# The same with the other index created in the aborted transaction.
	[
		sec_index_aborted => 'id int PRIMARY KEY, v int, k int',
		"CREATE INDEX ON T (k); UPDATE T SET k = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		deferrable =>
		  'id int, v int, UNIQUE (id) DEFERRABLE INITIALLY DEFERRED',
		"UPDATE T SET v = $X WHERE id = 3",
		'BEGIN; INSERT INTO T VALUES (3, 333); COMMIT', '23505'
	],
	[
		deferrable_key =>
		  'id int, v int, UNIQUE (id) DEFERRABLE INITIALLY DEFERRED',
		"UPDATE T SET id = $X WHERE id = 3",
		'BEGIN; INSERT INTO T VALUES (3, 333); COMMIT', '23505'
	],
	[
		gist_excl => 'id int, v int, EXCLUDE USING gist (id WITH =)',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23P01'
	],
	[
		gist_excl_key_old => 'id int, v int, EXCLUDE USING gist (id WITH =)',
		"UPDATE T SET id = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23P01'
	],
	[
		gist_excl_key_new => 'id int, v int, EXCLUDE USING gist (id WITH =)',
		"UPDATE T SET id = $X WHERE id = 3",
		"INSERT INTO T VALUES ($X, 333) RETURNING *", "ok:$X|333"
	],
	[
		btree_excl_key_old =>
		  'id int, v int, EXCLUDE USING btree (id WITH =)',
		"UPDATE T SET id = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23P01'
	],
	[
		btree_excl_key_new =>
		  'id int, v int, EXCLUDE USING btree (id WITH =)',
		"UPDATE T SET id = $X WHERE id = 3",
		"INSERT INTO T VALUES ($X, 333) RETURNING *", "ok:$X|333"
	],
	[
		hash_excl => 'id int, v int, EXCLUDE USING hash (id WITH =)',
		"UPDATE T SET v = $X WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23P01'
	],
	[
		upd_grow => 'id int PRIMARY KEY, v int, pad text',
		"UPDATE T SET v = $X, pad = repeat('x', 3000) WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		upd_del => 'id int PRIMARY KEY, v int',
		"UPDATE T SET v = $X WHERE id = 3; DELETE FROM T WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],
	[
		upd_twice => 'id int PRIMARY KEY, v int',
		"UPDATE T SET v = $X WHERE id = 3; UPDATE T SET id = 1003 WHERE id = 3",
		'INSERT INTO T VALUES (3, 333)', '23505'
	],);

# Run $sql with T replaced by the table name.
sub tab
{
	my ($sql, $t) = @_;
	$sql =~ s/\bT\b/$t/g;
	return $sql;
}

# Run a probe: "ok:" and its rows, or the SQLSTATE of its error.
sub probe
{
	my ($node, $sql) = @_;
	my ($out, $err);
	$node->psql(
		'postgres', "\\set VERBOSITY sqlstate\n$sql",
		stdout => \$out,
		stderr => \$err);
	return $1 if $err =~ /ERROR:\s+([0-9A-Z]{5})/;
	return "ok:" . join(',', split(/\n/, $out));
}

# Content of a table through a seqscan and through each index.
sub content
{
	my ($node, $t) = @_;
	my $q =
	  "SELECT count(*) || ':' || coalesce(string_agg(id || '/' || v, ',' ORDER BY id, v), '') FROM $t";
	return $node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; $q;\n"
		  . "SET enable_seqscan = off; SET enable_indexscan = on; $q;");
}

sub on_page
{
	my ($node, $t) = @_;
	return $node->safe_psql('postgres',
		"SELECT bool_or(position('\\x01020201'::bytea IN get_raw_page('$t', b::int)) > 0) FROM generate_series(0, pg_relation_size('$t') / 8192 - 1) b"
	);
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
));
$primary->start;
if (   !$primary->check_extension('injection_points')
	|| !$primary->check_extension('pageinspect')
	|| !$primary->check_extension('amcheck')
	|| !$primary->check_extension('btree_gist'))
{
	plan skip_all =>
	  'injection_points, pageinspect, amcheck or btree_gist not installed';
}
$primary->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
CREATE EXTENSION btree_gist;
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

my @all;    # every table, for the later checks

# One aborted transaction per case and access method, then the probe.
sub run_cases
{
	my ($mode) = @_;
	foreach my $c (@cases)
	{
		my ($name, $cols, $aborted, $probe, $expect) = @$c;
		my %res;
		foreach my $am ('flux', 'heap')
		{
			my $t = "${mode}_${name}_" . substr($am, 0, 1);
			push @all, $t;
			$primary->safe_psql('postgres',
					"CREATE TABLE $t ($cols) USING $am;\n"
				  . "INSERT INTO $t (id, v) SELECT g, g FROM generate_series(1, 5) g;\n"
				  . "BEGIN;\n"
				  . tab($aborted, $t)
				  . ";\nROLLBACK;");
			is(on_page($primary, $t), 't', "$t: the rollback is pending")
			  if $mode eq 'deferred' && $am eq 'flux' && $aborted =~ /$X/;
			$res{$am} = probe($primary, tab($probe, $t));
			is($res{$am}, $expect, "$t: probe");
		}
		my $f = "${mode}_${name}_f";
		(my $h = $f) =~ s/_f$/_h/;
		is($res{flux}, $res{heap}, "$f: probe as heap");
		is(content($primary, $f), content($primary, $h), "$f: rows as heap");
	}
}

# A uniqueness check against a writer that is still running must wait for
# it, and then decide by its outcome.
sub run_waits
{
	my ($mode) = @_;
	my $pk = 'id int PRIMARY KEY, v int';
	my $ins = 'INSERT INTO T VALUES (3, 333)';
	my $ocn = 'INSERT INTO T VALUES (3, 333) ON CONFLICT DO NOTHING';
	my $ocu =
	  'INSERT INTO T VALUES (3, 333) ON CONFLICT (id) DO UPDATE SET v = T.v + 1000';
	my $upd = "UPDATE T SET v = $X WHERE id = 3";
	my $updk = "UPDATE T SET id = $X WHERE id = 3";
	my $del = 'DELETE FROM T WHERE id = 3';
	my @waits = (
		[ 'upd_nonkey_abort', $pk, $upd, 'ROLLBACK', $ins, '23505' ],
		[ 'upd_key_abort', $pk, $updk, 'ROLLBACK', $ins, '23505' ],
		[ 'upd_nonkey_commit', $pk, $upd, 'COMMIT', $ins, '23505' ],
		[ 'upd_key_commit', $pk, $updk, 'COMMIT', $ins, 'ok:' ],
		[ 'del_abort', $pk, $del, 'ROLLBACK', $ins, '23505' ],
		[ 'del_commit', $pk, $del, 'COMMIT', $ins, 'ok:' ],
		[ 'upd_key_abort_ocn', $pk, $updk, 'ROLLBACK', $ocn, 'ok:' ],
		[ 'upd_key_commit_ocn', $pk, $updk, 'COMMIT', $ocn, 'ok:' ],
		[ 'upd_key_abort_ocu', $pk, $updk, 'ROLLBACK', $ocu, 'ok:' ],
		[ 'upd_key_commit_ocu', $pk, $updk, 'COMMIT', $ocu, 'ok:' ],
		[
			'uq_key_abort', 'id int, v int UNIQUE',
			"UPDATE T SET v = $X WHERE id = 3", 'ROLLBACK',
			'INSERT INTO T VALUES (33, 3)', '23505'
		],
		[
			'gist_key_abort', 'id int, v int, EXCLUDE USING gist (id WITH =)',
			$updk, 'ROLLBACK', $ins, '23P01'
		],
		[
			'gist_key_commit',
			'id int, v int, EXCLUDE USING gist (id WITH =)',
			$updk, 'COMMIT', $ins, 'ok:'
		],);

	foreach my $w (@waits)
	{
		my ($name, $cols, $writer, $end, $probe, $expect) = @$w;
		my %res;
		foreach my $am ('flux', 'heap')
		{
			my $t = "${mode}_wait_${name}_" . substr($am, 0, 1);
			push @all, $t;
			$primary->safe_psql('postgres',
					"CREATE TABLE $t ($cols) USING $am;\n"
				  . "INSERT INTO $t SELECT g, g FROM generate_series(1, 5) g;"
			);
			my $s1 = $primary->background_psql('postgres');
			my $s2 =
			  $primary->background_psql('postgres', on_error_stop => 0);
			$s1->query_safe("BEGIN; " . tab($writer, $t));
			$s2->query_safe("\\set VERBOSITY sqlstate");
			my $pid = $s2->query_safe('SELECT pg_backend_pid()');
			$s2->query_until(qr/go/, "\\echo go\n" . tab($probe, $t) . ";\n");
			ok( $primary->poll_query_until(
					'postgres',
					"SELECT wait_event_type = 'Lock' FROM pg_stat_activity WHERE pid = $pid"
				),
				"$t: the unique check waits for the writer");
			$s1->query_safe($end);
			$s2->query("SELECT 'synced'");
			$res{$am} =
			  $s2->{stderr} =~ /ERROR:\s+([0-9A-Z]{5})/ ? $1 : 'ok:';
			$s1->quit;
			$s2->quit;
			is($res{$am}, $expect, "$t: outcome after the writer ended");
		}
		my $f = "${mode}_wait_${name}_f";
		(my $h = $f) =~ s/_f$/_h/;
		is($res{flux}, $res{heap}, "$f: outcome as heap");
		is(content($primary, $f), content($primary, $h), "$f: rows as heap");
	}
}

# Inline rollback first, then with the undo apply worker held.
$primary->safe_psql('postgres',
	"ALTER SYSTEM SET undo_rollback_overflow_size = '1GB'; SELECT pg_reload_conf()"
);
run_cases('inline');
run_waits('inline');
$primary->safe_psql('postgres',
	"ALTER SYSTEM RESET undo_rollback_overflow_size; SELECT pg_reload_conf()"
);
$primary->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
run_cases('deferred');
run_waits('deferred');
ok( $primary->safe_psql(
		'postgres',
		"SELECT count(*) > 0 FROM pg_stat_activity WHERE wait_event = 'pbu-undo-worker-before-apply'"
	),
	'undo apply workers are held');

# A standby sees what the primary sees.
$primary->wait_for_replay_catchup($standby);
foreach my $t (@all)
{
	is(content($standby, $t), content($primary, $t), "$t: standby agrees");
}

# Let the undo apply workers go, then check again: same rows as the heap
# twin, every key still taken, every btree index intact.
$primary->safe_psql('postgres',
	"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
ok($primary->poll_query_until('postgres', 'SELECT release_one()'),
	'held undo apply workers released');
ok( $primary->poll_query_until(
		'postgres',
		'SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0'),
	'every rollback applied');
foreach my $t (grep { /_f$/ } @all)
{
	(my $h = $t) =~ s/_f$/_h/;
	is( content($primary, $t),
		content($primary, $h),
		"$t: rows as heap after the undo apply worker");
	is( $primary->safe_psql(
			'postgres',
			"WITH i AS (INSERT INTO $t SELECT * FROM $t ON CONFLICT DO NOTHING RETURNING 1) SELECT count(*) FROM i"
		),
		'0',
		"$t: unique checks see every row after the undo apply worker"
	) unless $t =~ /deferrable/;
	is( probe($primary, "INSERT INTO $t VALUES (3, 3)"),
		probe($primary, "INSERT INTO $h VALUES (3, 3)"),
		"$t: key 3 as heap after the undo apply worker");
	foreach my $i (
		split /\n/,
		$primary->safe_psql(
			'postgres',
			"SELECT i.indexrelid::regclass FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid JOIN pg_am a ON a.oid = c.relam WHERE i.indrelid = '$t'::regclass AND a.amname = 'btree'"
		))
	{
		my ($out, $err);
		my $rc = $primary->psql(
			'postgres',
			"SELECT bt_index_check('$i'::regclass, heapallindexed => true)",
			stdout => \$out,
			stderr => \$err);
		is("$rc:$err", '0:', "$i: bt_index_check");
	}
}
$primary->wait_for_replay_catchup($standby);
foreach my $t (@all)
{
	is( content($standby, $t),
		content($primary, $t),
		"$t: standby agrees after the undo apply worker");
}

foreach my $n ($primary, $standby)
{
	unlike(
		slurp_file($n->logfile),
		qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
		$n->name . ': no failed assertion, mismatched or abandoned undo');
}
$standby->stop;
$primary->stop;
done_testing();
