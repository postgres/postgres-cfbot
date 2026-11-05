# Copyright (c) 2021-2026, PostgreSQL Global Development Group
#
# Promote a standby that stopped at a recovery target while its WAL
# summarizer was waiting for more WAL.  The summarizer must notice that the
# old timeline became historic and continue on the new timeline from the
# switch point.  A standby summarizes only replayed WAL, so the summarizer
# waits at the switch point here, although the standby has received WAL past
# it.  When a standby summarized what the walreceiver had flushed, the switch
# point could precede the start of the summary being written, which used to
# trip an assertion.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', <<EOM);
autovacuum = off
summarize_wal = on
EOM
$primary->start;

$primary->safe_psql('postgres', 'CREATE TABLE t (i int)');
$primary->backup('backup');

# The recovery target is the redo pointer of a checkpoint; the standby also
# receives a second checkpoint past it.
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (1); CHECKPOINT;');
my $target_lsn = $primary->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (2); CHECKPOINT;');
my $end_lsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', <<EOM);
recovery_target_lsn = '$target_lsn'
recovery_target_inclusive = false
recovery_target_action = 'pause'
log_min_messages = debug1
EOM
$standby->start;

# Wait until replay is paused at the target, the standby has received the
# WAL past it, and the summarizer has reached replay and is waiting for more
# WAL.
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for recovery to pause";
$standby->poll_query_until('postgres',
	"SELECT pg_last_wal_receive_lsn() >= '$end_lsn'")
  or die "timed out waiting for the standby to receive up to $end_lsn";
$standby->poll_query_until('postgres',
	"SELECT pending_lsn >= pg_last_wal_replay_lsn() FROM pg_get_wal_summarizer_state()"
) or die "timed out waiting for summarization to reach replay";
$standby->poll_query_until('postgres', <<EOM)
SELECT EXISTS (SELECT FROM pg_stat_activity
               WHERE backend_type = 'walsummarizer'
               AND wait_event = 'WalSummarizerWal')
EOM
  or die "timed out waiting for summarizer to wait for WAL";
is( $standby->safe_psql(
		'postgres',
		"SELECT pending_lsn <= pg_last_wal_replay_lsn() FROM pg_get_wal_summarizer_state()"
	),
	't',
	'the summarizer does not read past replay');

my $log_offset = -s $standby->logfile;
$standby->promote;
$standby->safe_psql('postgres', 'CHECKPOINT');

# Summarization must end on timeline 1 and resume on timeline 2 exactly at
# the switch point.
my $result = $standby->poll_query_until('postgres', <<EOM);
SELECT EXISTS (SELECT FROM pg_available_wal_summaries()
               WHERE tli = 2 AND start_lsn = '$target_lsn')
EOM
ok($result, "summarization resumed on timeline 2 at $target_lsn");
is( $standby->safe_psql(
		'postgres',
		"SELECT max(end_lsn) FROM pg_available_wal_summaries() WHERE tli = 1"),
	$target_lsn,
	"timeline 1 summaries end at $target_lsn");

ok( !$standby->log_contains(
		qr/WAL summarizer process .* was terminated by signal/, $log_offset),
	'no WAL summarizer crash after promotion');

done_testing();
