# Copyright (c) 2026, PostgreSQL Global Development Group

# A WAL read served from WAL buffers must not leave a stale open segment
# behind for a later file read. Exercised for read_local_xlog_page() (via
# pg_walinspect) and the logical walsender (via pg_recvlogical).

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 'logical');
# Keep recent WAL in buffers so the new segment's first page is read from
# buffers rather than from a file.
$node->append_conf('postgresql.conf', 'wal_buffers = 64MB');
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('slot', 'test_decoding')");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('wal-read-from-buffers-force-miss', 'notice')");

# Emit one message per WAL page in a segment, plus a few more, so that the WAL
# written after the switch crosses a segment boundary by a couple of pages.
my $seg_size = $node->safe_psql('postgres',
	"SELECT pg_size_bytes(current_setting('wal_segment_size'))");
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $start_lsn = $node->safe_psql('postgres', 'SELECT pg_current_wal_lsn()');
$node->safe_psql('postgres',
	"SELECT count(pg_logical_emit_message(false, 'test', repeat('x', 8192)))
	 FROM generate_series(1, $seg_size / 8192 + 16)");
my $end_lsn = $node->safe_psql('postgres',
	"SELECT pg_logical_emit_message(false, 'test', 'flush', true)");

# read_local_xlog_page() path.
my ($ret, $stdout, $stderr) = $node->psql('postgres',
	"SELECT count(*) > 0 FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')");
is($ret, 0, 'pg_walinspect reads across a segment boundary');
is($stdout, 't', 'pg_walinspect returns records across the boundary');

# logical walsender path.
my ($rc, $rout, $rerr) = $node->pg_recvlogical_upto('postgres', 'slot',
	$end_lsn, $PostgreSQL::Test::Utils::timeout_default);
is($rc, 0, 'walsender decodes across a segment boundary');
unlike($rerr, qr/unexpected pageaddr/, 'walsender did not reuse a stale segment');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('wal-read-from-buffers-force-miss')");

done_testing();
