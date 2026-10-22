# Copyright (c) 2021-2026, PostgreSQL Global Development Group
#
# A standby that stops at a recovery target can have summarized WAL beyond
# that target, because the WAL summarizer reads whatever the walreceiver has
# flushed. When such a standby is promoted, the old timeline ends at a point
# the summarizer has already passed. The summarizer must back up to the switch
# point and continue on the new timeline, rather than report a summary that
# ends before it started (which used to trip an assertion).

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

# The recovery target is the redo pointer of a checkpoint, so that the
# standby's summarizer completes a summary that starts exactly there before
# promotion; a second checkpoint lets it summarize past the target.
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (1); CHECKPOINT;');
my $target_lsn = $primary->safe_psql('postgres',
	'SELECT redo_lsn FROM pg_control_checkpoint()');
$primary->safe_psql('postgres', 'INSERT INTO t VALUES (2); CHECKPOINT;');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->append_conf('postgresql.conf', <<EOM);
recovery_target_lsn = '$target_lsn'
recovery_target_inclusive = false
recovery_target_action = 'pause'
log_min_messages = debug1
EOM
$standby->start;

# Wait until replay is paused at the target, the summarizer has summarized
# past it on timeline 1, and the summarizer is waiting for more WAL.
$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "timed out waiting for recovery to pause";
$standby->poll_query_until('postgres', <<EOM)
SELECT EXISTS (SELECT FROM pg_available_wal_summaries()
               WHERE tli = 1 AND start_lsn = '$target_lsn')
EOM
  or die "timed out waiting for summary starting at $target_lsn";
$standby->poll_query_until('postgres', <<EOM)
SELECT EXISTS (SELECT FROM pg_stat_activity
               WHERE backend_type = 'walsummarizer'
               AND wait_event = 'WalSummarizerWal')
EOM
  or die "timed out waiting for summarizer to wait for WAL";

my $log_offset = -s $standby->logfile;
$standby->promote;
$standby->safe_psql('postgres', 'CHECKPOINT');

# Summarization must resume on timeline 2 exactly at the switch point.
my $result = $standby->poll_query_until('postgres', <<EOM);
SELECT EXISTS (SELECT FROM pg_available_wal_summaries()
               WHERE tli = 2 AND start_lsn = '$target_lsn')
EOM
ok($result, "summarization resumed on timeline 2 at $target_lsn");

ok( !$standby->log_contains(
		qr/WAL summarizer process .* was terminated by signal/, $log_offset),
	'no WAL summarizer crash after promotion');

done_testing();
