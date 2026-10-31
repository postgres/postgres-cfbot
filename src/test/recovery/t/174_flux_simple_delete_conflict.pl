
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX simple index deletion must log a snapshotConflictHorizon, as heap does.
#
# When nbtree removes dead index entries by "simple deletion" (the LP_DEAD
# entries kill_prior_tuple leaves behind, swept by _bt_simpledel_pass), it
# asks the table AM for a snapshotConflictHorizon via
# table_index_delete_tuples() and logs it in the XLOG_BTREE_DELETE record.  A
# hot standby replays that record and cancels any snapshot that could still
# see the removed entries (recovery conflict); with hot_standby_feedback=off
# that horizon is the only thing that protects such a standby query from
# losing index entries it can still see.
#
# flux_index_delete_tuples() returned InvalidTransactionId (0) unconditionally,
# so the btree DELETE record carried snapshotConflictHorizon 0: the standby
# never conflicts, and a REPEATABLE READ query there can miss rows.
#
# This test reproduces the defect at the WAL level -- the mechanism, not a
# timing race: it forces FLUX index entries dead (kill_prior_tuple) and then
# simple-deleted, and asserts the resulting Btree DELETE record carries a
# non-zero snapshotConflictHorizon.  On the unfixed code it is 0 (RED).  It
# also runs a primary/standby end-to-end check: a held REPEATABLE READ query
# on a hot_standby_feedback=off standby that read the rows through the index
# must, after the removal is replayed, either be cancelled or still see its
# rows -- never silently lose them.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my (undef, $stderr) =
  run_command [qw(postgres -C invalid -c io_method=invalid)];
my $io_method =
  ($stderr =~ m/Available values:[^\.]*\bio_uring\b/) ? 'io_uring' : 'worker';

my $primary = PostgreSQL::Test::Cluster->new('sd_primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"io_method = $io_method\n"
	  . "wal_consistency_checking = all\n"
	  . "autovacuum = off\n");
$primary->start;

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('sd_standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf(
	'postgresql.conf',
	"io_method = $io_method\n"
	  . "hot_standby_feedback = off\n"
	  . "max_standby_streaming_delay = 0\n"
	  . "max_standby_archive_delay = 0\n");
$standby->start;

# A FLUX table with a secondary btree index; delete half the rows (committed)
# so their index entries become removable by simple deletion.
$primary->safe_psql(
	'postgres', q{
CREATE TABLE sd (id int PRIMARY KEY, v int) USING flux;
INSERT INTO sd SELECT g, g FROM generate_series(1, 800) g;
CREATE INDEX sd_v ON sd(v);
DELETE FROM sd WHERE id % 2 = 0;
});
$primary->wait_for_replay_catchup($standby);

# Hold a REPEATABLE READ snapshot on the standby that reads the soon-to-be-
# removed rows through the index.  on_error_stop => 0 so a recovery-conflict
# ERROR is an inspectable result, not a die.
my $s = $standby->background_psql('postgres', on_error_stop => 0);
$s->query_safe('SET enable_seqscan = off');
$s->query_safe('SET enable_bitmapscan = off');
$s->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ');
my $old = $s->query_safe(
	'SELECT count(*) FROM (SELECT v FROM sd WHERE v BETWEEN 1 AND 800) q');
is($old, 400, 'held standby snapshot sees the 400 surviving rows via the index');

my $log_pos = -s $standby->logfile;
my $lsn_before = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

# On the primary, force kill_prior_tuple to mark the dead entries LP_DEAD and
# then trigger simple deletion (_bt_simpledel_pass) with inserts onto the same
# leaf pages.
$primary->safe_psql(
	'postgres', q{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM (SELECT v FROM sd WHERE v BETWEEN 1 AND 800 ORDER BY v) q;
SELECT count(*) FROM (SELECT v FROM sd WHERE v BETWEEN 1 AND 800 ORDER BY v) q;
INSERT INTO sd SELECT g, g FROM generate_series(1001, 1800) g;
SELECT count(*) FROM (SELECT v FROM sd WHERE v BETWEEN 1 AND 800 ORDER BY v) q;
INSERT INTO sd SELECT g, g FROM generate_series(1801, 2600) g;
});
my $lsn_after = $primary->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');

# RED->GREEN at the WAL level: at least one Btree DELETE record over the dead
# FLUX index entries must carry a non-zero snapshotConflictHorizon.  On the
# unfixed code every such record carried 0.
my $wal = $primary->data_dir . '/pg_wal';
my ($wout, $werr) = run_command(
	[ 'pg_waldump', '-p', $wal, '-s', $lsn_before, '-e', $lsn_after, '-r', 'Btree' ]);
my $max_horizon = 0;
my $saw_delete = 0;
for my $line (split /\n/, $wout)
{
	next unless $line =~ /desc: DELETE /;
	next unless $line =~ /snapshotConflictHorizon: (\d+)/;
	$saw_delete = 1;
	$max_horizon = $1 if $1 > $max_horizon;
}
ok($saw_delete, 'simple deletion produced a Btree DELETE record for FLUX')
  or diag($wout);
ok($max_horizon > 0,
	'FLUX simple-deletion Btree DELETE record carries a non-zero snapshotConflictHorizon')
  or diag("max_horizon=$max_horizon\n$wout");

$primary->wait_for_replay_catchup($standby);

# End-to-end: the held snapshot must not silently lose rows.  A recovery
# conflict may arrive as an ERROR (session survives) or a FATAL (connection
# terminated); either is a valid cancellation.  Otherwise it must still see
# all 400 rows.
my ($out, $errored) = (undef, 0);
my $session_alive = 1;
eval { ($out, $errored) = $s->query(
		'SELECT count(*) FROM (SELECT v FROM sd WHERE v BETWEEN 1 AND 800) q'); 1; }
  or do { $session_alive = 0; };

my $standby_log = slurp_file($standby->logfile, $log_pos);
my $cancelled = $errored || !$session_alive
  || $standby_log =~ /conflict with recovery/;
my $still_all = (defined $out && $out =~ /\b400\b/);
ok($cancelled || $still_all,
	'held standby read was cancelled or still saw all its rows (never lost entries)')
  or diag("errored=$errored alive=$session_alive out=" . (defined $out ? $out : '<undef>'));

eval { if ($session_alive) { $s->quit; } else { $s->{run}->finish; } 1; };

$standby->stop;
$primary->stop;
done_testing();
