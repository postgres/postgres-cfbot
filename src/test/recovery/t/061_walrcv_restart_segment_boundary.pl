
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that restarting the walreceiver while a standby is caught up to a
# position just short of a WAL segment boundary does not skip the tail of
# that segment.
#
# When the last record received ends SizeOfXLogRecord bytes or fewer
# before the end of a segment, the next record header reaches the segment
# boundary, so the startup process waits for WAL up to exactly the start
# of the next segment.  If the walreceiver is restarted at that
# point (here by changing primary_conninfo), streaming must resume in the
# segment holding the last byte recovery needs, not in the next one;
# otherwise the end of the current segment is never fetched.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
# Keep the primary quiet, so that nothing writes WAL between the padding
# records emitted below and the walreceiver restart.
$primary->append_conf(
	'postgresql.conf', qq(
autovacuum = off
checkpoint_timeout = 1h
wal_keep_size = 128MB
));
$primary->start;

my $backup_name = 'my_backup';
$primary->backup($backup_name);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
$standby->append_conf('postgresql.conf',
	'wal_retrieve_retry_interval = 100ms');
$standby->start;

$primary->safe_psql('postgres', 'CREATE TABLE test_tab (a int)');
$primary->wait_for_catchup($standby);

my $wal_block_size = $primary->safe_psql('postgres',
	"SELECT setting::int FROM pg_settings WHERE name = 'wal_block_size'");
my $wal_segment_size = $primary->safe_psql('postgres',
	"SELECT setting::int FROM pg_settings WHERE name = 'wal_segment_size'");

# Bytes left in the segment after the last record.  At most
# SizeOfXLogRecord, so that the next record header reaches the boundary.
my $tail_gap = 8;

# Emit a non-transactional logical message, so that no commit record
# follows it.  Returns the end LSN of the record, in bytes.
sub emit_message
{
	my ($node, $size) = @_;

	return int(
		$node->safe_psql(
			'postgres',
			"SELECT pg_logical_emit_message(false, '', repeat('a', $size)) - '0/0'"
		));
}

sub insert_lsn
{
	my ($node) = @_;

	return int(
		$node->safe_psql(
			'postgres', "SELECT pg_current_wal_insert_lsn() - '0/0'"));
}

# Emit WAL records until the last one ends exactly at $target.  The last
# two records are emitted entirely within the page of $target, so their
# size depends only on the message length; the first is used to measure
# the record overhead.  Returns false if $target was overshot.
sub pad_wal_to
{
	my ($node, $target) = @_;
	my $target_page = int($target / $wal_block_size);
	my $calibration_size = 500;

	while (1)
	{
		my $pos = insert_lsn($node);
		my $remaining = $target - $pos;

		return 0 if $remaining < 0;

		# Get onto the target page in big steps, leaving room for two
		# records.  Leave some headroom for the page headers a large record
		# spans.
		if (int($pos / $wal_block_size) != $target_page
			|| $remaining > $wal_block_size / 2)
		{
			my $size = $remaining - int($remaining / 256) - 2500;
			emit_message($node, $size > 1 ? $size : 1);
			next;
		}

		return 0 if $remaining < 1200;

		# Records are MAXALIGN'd, so a record carrying $calibration_size +
		# N bytes of data occupies exactly N bytes more than one carrying
		# $calibration_size bytes, when N is a multiple of MAXALIGN.  Both
		# are large enough to use the long data header, so this holds for
		# any MAXALIGN.
		my $start = insert_lsn($node);
		my $overhead = emit_message($node, $calibration_size) - $start;
		$remaining = $target - insert_lsn($node);
		my $end =
		  emit_message($node, $calibration_size + $remaining - $overhead);
		return $end == $target;
	}
}

sub lsn_to_text
{
	my ($lsn) = @_;

	return sprintf('%X/%08X', $lsn >> 32, $lsn & 0xFFFFFFFF);
}

# Position at which a new walreceiver starts streaming.  If it is ahead of
# the primary's flush position, the walreceiver logs it and waits.
my $start_point_re =
  qr/(?:started streaming WAL from primary at|walreceiver requested start point) ([0-9A-F]+)\/([0-9A-F]+) /;

# Background activity can write WAL between the padding and the walreceiver
# restart and spoil the setup, so retry a few times if that happens.
my $boundary;
my $target;
my $log_offset;
my $attempt;
for ($attempt = 1; $attempt <= 5; $attempt++)
{
	$boundary =
	  (int(insert_lsn($primary) / $wal_segment_size) + 2) * $wal_segment_size;
	$target = $boundary - $tail_gap;
	if (!pad_wal_to($primary, $target))
	{
		note "attempt $attempt: could not end WAL at " . lsn_to_text($target);
		next;
	}

	# Wait for the standby to receive and replay everything up to $target.
	$primary->poll_query_until('postgres',
		"SELECT pg_current_wal_flush_lsn() >= '" . lsn_to_text($target) . "'")
	  or die "timed out waiting for primary to flush WAL";
	$standby->poll_query_until('postgres',
		"SELECT pg_last_wal_replay_lsn() >= '" . lsn_to_text($target) . "'")
	  or die "timed out waiting for standby to replay WAL";

	# Retry unless the standby stopped exactly at $target.
	my $flushed = $standby->safe_psql('postgres',
		"SELECT flushed_lsn - '0/0' FROM pg_stat_wal_receiver");
	if ($flushed != $target)
	{
		note "attempt $attempt: standby received up to "
		  . lsn_to_text($flushed)
		  . ", not "
		  . lsn_to_text($target);
		next;
	}

	# Restart the walreceiver by changing primary_conninfo.  Leave
	# application_name alone, as wait_for_catchup() relies on it.
	$log_offset = -s $standby->logfile;
	my $conninfo = $standby->safe_psql('postgres', 'SHOW primary_conninfo');
	$conninfo .= " connect_timeout=" . (60 + $attempt);
	$standby->safe_psql('postgres',
		"ALTER SYSTEM SET primary_conninfo = '$conninfo'");
	$standby->reload;
	$standby->wait_for_log($start_point_re, $log_offset);
	last;
}
ok($attempt <= 5, 'last record ends just before a segment boundary');

# Streaming must restart in the segment holding $target, not at $boundary.
my $log = slurp_file($standby->logfile, $log_offset);
$log =~ $start_point_re or die "no walreceiver start point in standby log";
is( lsn_to_text(hex($1) << 32 | hex($2)),
	lsn_to_text($boundary - $wal_segment_size),
	'walreceiver restarts streaming at the start of the current segment');

# Write WAL across the boundary, and check that the standby replays it.
$primary->safe_psql('postgres',
	'INSERT INTO test_tab SELECT generate_series(1, 1000)');
$primary->wait_for_catchup($standby);
is($standby->safe_psql('postgres', 'SELECT count(*) FROM test_tab'),
	'1000', 'standby replays WAL crossing the segment boundary');

done_testing();
