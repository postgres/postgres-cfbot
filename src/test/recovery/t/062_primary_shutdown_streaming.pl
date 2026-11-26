# Copyright (c) 2026, PostgreSQL Global Development Group

# Clean primary shutdown must not FATAL in endstreaming, reuse the dead
# connection for IDENTIFY_SYSTEM, or archive an incomplete WAL segment
# under archive_mode=always. Streaming must resume after restart.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use Test::More;

# pg_walfile_name() is unavailable during recovery. Assumes the default
# 16MB wal_segment_size used by the TAP suite.
sub walfile_from_lsn
{
	my ($lsn, $tli) = @_;
	$tli = 1 unless defined $tli;
	my ($hi_hex, $lo_hex) = split(m{/}, $lsn);
	my $hi = hex($hi_hex);
	my $lo = hex($lo_hex);
	my $segsize = 16 * 1024 * 1024;
	my $segs_per_id = 256;    # 2^32 / 16MB
	my $logSegNo = $hi * $segs_per_id + int($lo / $segsize);
	return sprintf("%08X%08X%08X", $tli, int($logSegNo / $segs_per_id),
		$logSegNo % $segs_per_id);
}

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->start;
$node_primary->backup('backup');

my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup(
	$node_primary, 'backup',
	has_streaming => 1);
# init_from_backup ignores has_archiving. Set up standby archiving by hand.
$node_standby->enable_archiving;
$node_standby->append_conf('postgresql.conf', 'archive_mode = always');
$node_standby->start;

# Finish one segment, then leave the next one incomplete.
$node_primary->safe_psql('postgres',
	"CREATE TABLE shutdown_stream_tab AS SELECT generate_series(1, 100) AS id");
$node_primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$node_primary->safe_psql('postgres',
	"INSERT INTO shutdown_stream_tab VALUES (101)");
$node_primary->wait_for_catchup($node_standby);

my $log_offset = -s $node_standby->logfile;
$node_primary->stop;

# Wait until a successor walreceiver fails to connect. That means the old
# process finished endstreaming and exited.
$node_standby->wait_for_log(qr/could not connect to the primary server/,
	$log_offset);

ok( !$node_standby->log_contains(
		'could not send end-of-streaming message', $log_offset),
	'walreceiver does not fail ending the stream after primary shutdown');
ok( !$node_standby->log_contains(
		'could not receive database system identifier and timeline ID',
		$log_offset),
	'walreceiver does not reuse the closed connection for IDENTIFY_SYSTEM');

# Shutdown checkpoint may complete the pre stop segment. Check the one that
# remains incomplete afterwards.
my $receive_lsn = $node_standby->safe_psql('postgres',
	"SELECT pg_last_wal_receive_lsn()");
my $incomplete_seg = walfile_from_lsn($receive_lsn);
my $archive_status =
  $node_standby->data_dir . "/pg_wal/archive_status/$incomplete_seg.ready";
ok( !-f $archive_status,
	"incomplete segment $incomplete_seg does not get a .ready file");
ok( !-f $node_standby->archive_dir . "/$incomplete_seg",
	"incomplete segment $incomplete_seg is not archived while primary is down");

my $reconnect_offset = -s $node_standby->logfile;
$node_primary->start;
$node_standby->wait_for_log(
	qr/(?:started streaming WAL|restarted WAL streaming)/,
	$reconnect_offset);
$node_primary->safe_psql('postgres',
	"INSERT INTO shutdown_stream_tab VALUES (102)");
$node_primary->wait_for_catchup($node_standby);

my $result =
  $node_standby->safe_psql('postgres',
	"SELECT count(*) FROM shutdown_stream_tab");
is($result, '102', 'standby reconnects and catches up after primary restart');

$node_standby->stop;
$node_primary->stop;

done_testing();
