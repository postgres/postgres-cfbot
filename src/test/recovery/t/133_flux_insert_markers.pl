
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX inserts are judged by t_xmin, as heap's are; no per-tuple marker in
# shared memory is needed.  Heap is the control for every case.
#
# Redo used to register every uncommitted inserted FLUX tuple in the shared
# tuple sLog, which a standby never needed: t_xmin, CLOG and the
# known-assigned xids decide visibility there.  Once that fixed table filled,
# replay logged one "SLOG_LOST_OP" WARNING per tuple (half a million for two
# 400k-row inserts).
#
# A rolled-back subtransaction's inserts, and a speculative insert, were also
# given shared markers.  t_xmin is the subtransaction's xid, which CLOG
# records as aborted, and a speculative insert keeps its token in t_ctid as
# heap's does; the cases below check unique and ON CONFLICT inserts that meet
# such rows.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"autovacuum = off\nmax_connections = 20");
$primary->start;
# A slot keeps the WAL the standby still needs: the inserts below write a
# lot of it, more still under wal_consistency_checking.
$primary->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('standby')");
$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf', "primary_slot_name = 'standby'");
$standby->start;

my $n = 400000;
foreach my $am ('heap', 'flux')
{
	$primary->safe_psql('postgres',
		"CREATE TABLE t_$am (id int) USING $am; INSERT INTO t_$am VALUES (0);"
	);

	# Two uncommitted inserts: one will commit, one will roll back.
	my $a = $primary->background_psql('postgres');
	my $b = $primary->background_psql('postgres');
	$a->query_safe('BEGIN');
	$a->query_safe("INSERT INTO t_$am SELECT generate_series(1, $n)");
	$b->query_safe('BEGIN');
	$b->query_safe("INSERT INTO t_$am SELECT generate_series(1, $n)");
	$primary->wait_for_replay_catchup($standby);
	is($standby->safe_psql('postgres', "SELECT count(*) FROM t_$am"),
		'1', "$am: standby does not see uncommitted inserts");

	$a->query_safe('COMMIT');
	$b->query_safe('ROLLBACK');
	$a->quit;
	$b->quit;
	$primary->wait_for_replay_catchup($standby);
	is($standby->safe_psql('postgres', "SELECT count(*) FROM t_$am"),
		$n + 1, "$am: standby sees the committed insert only");
}

# Unique checks against rows of an open transaction's rolled-back
# subtransaction, and against an in-progress speculative insert.
foreach my $am ('heap', 'flux')
{
	$primary->safe_psql('postgres',
		"CREATE TABLE u_$am (k int PRIMARY KEY) USING $am");
	my $a = $primary->background_psql('postgres', on_error_stop => 0);
	my $try = sub {
		my ($sql) = @_;
		my ($ret, $out, $err) =
		  $primary->psql('postgres', "SET lock_timeout = '1s'; $sql");
		return $err =~ /lock timeout/ ? 'waits' : $ret == 0 ? 'ok' : $err;
	};

	$a->query_safe('BEGIN');
	$a->query_safe('SAVEPOINT s; INSERT INTO u_'
		  . $am
		  . ' VALUES (1), (2); ROLLBACK TO s');
	is($a->query_safe("SELECT count(*) FROM u_$am"),
		'0', "$am: rows of a rolled-back savepoint are gone");
	is($try->("INSERT INTO u_$am VALUES (1)"),
		'ok', "$am: unique insert does not wait for a rolled-back savepoint");
	$a->query_safe('SAVEPOINT s; INSERT INTO u_'
		  . $am
		  . ' VALUES (4) ON CONFLICT DO NOTHING; ROLLBACK TO s');
	is($try->("INSERT INTO u_$am VALUES (4) ON CONFLICT DO NOTHING"), 'ok',
		"$am: ON CONFLICT does not wait for a rolled-back speculative insert"
	);
	$a->query_safe("INSERT INTO u_$am VALUES (3) ON CONFLICT DO NOTHING");
	is($try->("INSERT INTO u_$am VALUES (3) ON CONFLICT DO NOTHING"),
		'waits',
		"$am: ON CONFLICT waits for an in-progress speculative insert");
	$a->query_safe("INSERT INTO u_$am VALUES (2) ON CONFLICT DO NOTHING");
	$a->query_safe('ROLLBACK');
	is($try->("INSERT INTO u_$am VALUES (3) ON CONFLICT DO NOTHING"),
		'ok',
		"$am: ON CONFLICT inserts after the speculative inserter aborts");
	is( $primary->safe_psql(
			'postgres',
			"SELECT string_agg(k::text, ',' ORDER BY k) FROM u_$am"),
		'1,3,4',
		"$am: final contents");
	$a->quit;
}

my $log = slurp_file($standby->logfile);
my $warnings = () = $log =~ /WARNING/g;
is($warnings, 0, 'standby logged no WARNING during replay');

done_testing();
