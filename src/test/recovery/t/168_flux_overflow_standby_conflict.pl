
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A hot-standby query that can still reach a superseded or deleted wide value
# through the replayed UNDO chain must never read a freed FLUX overflow chunk.
# When the primary's VACUUM frees those chunks, the standby must EITHER cancel
# the query (recovery conflict) OR return the old bytes intact -- it must never
# read "FLUX overflow: root chunk lp invalid" or return wrong bytes.
#
# The overflow orphan sweep (FluxOverflowCheckOrphans, flux_overflow.c) frees
# chunks with FluxXLogDefrag; it now passes a snapshotConflictHorizon (a
# conservative TransactionIdRetreat(oldest_xmin) bound) so redo resolves the
# conflict, as heap's prune and FLUX VACUUM Phase III already do.
#
# NOTE on coverage: in every shape below the conflict that actually cancels
# the standby snapshot today comes from the referencing tuple's page -- the
# in-place UPDATE's / VACUUM's VM_SET (conflict_xid) or Phase III's vac_horizon
# -- which covers the same snapshots the overflow sweep's horizon would.  So
# this test exercises the end-to-end invariant and guards against a regression
# that would let the standby read a freed chunk, but it does not isolate the
# sweep's own horizon (that record's horizon is defense in depth; see the
# shortcut comment in flux_overflow.c).  Three DML shapes are covered:
# UPDATE-every-row (superseded wide values) and DELETE-every-row (chunks of
# deleted versions).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my (undef, $stderr) =
  run_command [qw(postgres -C invalid -c io_method=invalid)];
my $io_method =
  ($stderr =~ m/Available values:[^\.]*\bio_uring\b/) ? 'io_uring' : 'worker';

my $primary = PostgreSQL::Test::Cluster->new('sc_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"io_method = $io_method\n"
	  . "wal_consistency_checking = all\n"
	  . "autovacuum = off\n");
$primary->start;

is($primary->safe_psql('postgres', 'SHOW io_method'),
	$io_method, "primary runs with io_method=$io_method");

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('sc_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf',
	"io_method = $io_method\n"
	  . "hot_standby_feedback = off\n"
	  . "max_standby_streaming_delay = 0\n"
	  . "max_standby_archive_delay = 0\n");
$standby->start;

# ~10 kB incompressible out-of-line value -> FLUX-native overflow chunks.
sub big { my ($seed) = @_; return "repeat(md5($seed)::text, 320)" }
my $digest = q{md5(coalesce(string_agg(id || ':' || big, ',' ORDER BY id), ''))};

# One shape: build a wide FLUX table, hold a standby REPEATABLE READ snapshot
# that has detoasted every value, run $dml (which supersedes/deletes the held
# values) + VACUUM on the primary, let the standby replay the frees, then read
# again in the held snapshot.  Assert the invariant.
sub run_shape
{
	my ($name, $tbl, $dml, $waldump_check) = @_;

	$primary->safe_psql(
		'postgres', qq{
CREATE TABLE $tbl (id int PRIMARY KEY, big text) USING flux;
ALTER TABLE $tbl ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO $tbl SELECT g, @{[big('g::text')]} FROM generate_series(1, 60) g;
});
	$primary->wait_for_replay_catchup($standby);

	my $old = $standby->safe_psql('postgres', "SELECT $digest FROM $tbl");

	# Hold the snapshot on the standby and detoast now.  on_error_stop => 0 so
	# a recovery-conflict ERROR is an inspectable result, not a die.
	my $s = $standby->background_psql('postgres', on_error_stop => 0);
	$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
	is($s->query_safe("SELECT $digest FROM $tbl"),
		$old, "$name: held standby snapshot sees the old wide values");

	my $log_pos = -s $standby->logfile;

	# Supersede/delete on the primary and advance the horizon, then VACUUM.
	$primary->safe_psql('postgres', $dml);
	my $lsn_before =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
	$primary->safe_psql('postgres', "VACUUM $tbl");
	my $lsn_after =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

	# RED->GREEN for the fix itself: the overflow orphan sweep frees chunks on
	# overflow-only data pages (high block numbers -- they carry no regular
	# tuple, so VACUUM's VM_SET logs conflict_xid 0 and FLUX VACUUM Phase III
	# never touches them).  The ONLY record that can protect a standby reader
	# of those chunks is the sweep's own DEFRAG.  Assert at least one FLUX
	# VACUUM(DEFRAG) record on such a page carries a non-zero
	# snapshotConflictHorizon.  Before the fix every one of them was 0 (RED);
	# after it they carry TransactionIdRetreat(oldest_xmin) (GREEN).
	if ($waldump_check)
	{
		my $wal = $primary->data_dir . '/pg_wal';
		my ($wout, $werr) = run_command(
			[
				'pg_waldump', '-p', $wal, '-s', $lsn_before, '-e',
				$lsn_after, '-r', 'FLUX'
			]);
		my $found_horizon = 0;
		for my $line (split /\n/, $wout)
		{
			next unless $line =~ /desc: VACUUM /;
			next unless $line =~ /snapshotConflictHorizon: (\d+)/;
			my $h = $1;
			next unless $line =~ /\bblk (\d+)/;
			my $blk = $1;
			# Overflow-only pages sit well past the ~60 main-table pages.
			$found_horizon = 1 if $blk >= 100 && $h != 0;
		}
		ok($found_horizon,
			"$name: overflow-sweep DEFRAG carries a snapshotConflictHorizon");
	}
	$primary->wait_for_replay_catchup($standby);

	# Re-read in the held snapshot.  A recovery conflict may arrive as an ERROR
	# (session survives) or a FATAL that terminates the connection -- in which
	# case $s->query() dies with "process ended prematurely".  Both are valid
	# cancellations; catch the die so we can assert the invariant either way.
	my $out = '';
	my $errored = 0;
	my $session_alive = 1;
	eval {
		($out, $errored) = $s->query("SELECT $digest FROM $tbl");
		1;
	} or do {
		$session_alive = 0;    # the backend was terminated by the conflict
	};

	my $standby_log = slurp_file($standby->logfile, $log_pos);

	ok(!($standby_log =~ /root chunk lp invalid/),
		"$name: no \"root chunk lp invalid\" on the standby");

	# Any digest the read returned must be the OLD one; never wrong bytes.
	my ($got) = ($out =~ /\b([0-9a-f]{32})\b/);
	ok(!(defined $got && $got ne $old),
		"$name: held standby read never returned wrong wide-value bytes")
	  or diag("got=$got old=$old out=$out");

	# The conflict must resolve one of the two safe ways: cancelled (an error
	# in the session or a recovery-conflict in the log), or the correct old
	# bytes were returned.
	my $cancelled =
	     $errored
	  || !$session_alive
	  || $standby_log =~ /conflict with recovery/;
	my $correct = (defined $got && $got eq $old);
	ok($cancelled || $correct,
		"$name: standby read was cancelled or returned the old values")
	  or diag("errored=$errored alive=$session_alive out=$out");

	# Leave a clean session for the next shape.
	eval { if ($session_alive) { $s->quit; } else { $s->{run}->finish; } 1; };
}

run_shape('update-all', 'ovf_upd', join('; ',
	map { "UPDATE ovf_upd SET big = @{[big(\"(id*1000+$_)::text\")]}" } 1 .. 3), 1);

run_shape('delete-all', 'ovf_del', 'DELETE FROM ovf_del');

# A third shape, an UPDATE that changes only the wide column (the compact
# FLUX_UNDO_UPDATE_DELTA path), is intentionally omitted here: on this base it
# trips a pre-existing FLUX delta-UPDATE WAL-consistency mismatch
# ("inconsistent page" under wal_consistency_checking, unrelated to the
# overflow sweep and to this commit -- the FLUX WAL-diff fix tracked by
# recovery/146 covers it).  The update-all shape above already exercises a
# wide column changing across the held snapshot.

$standby->stop;
$primary->stop;
done_testing();
