# Copyright (c) 2025-2026, PostgreSQL Global Development Group
#
# Test whole-record WAL compression via wal_compression and
# wal_compression_threshold.  Exercises compression during replication
# (walsender path) and decompression during crash recovery (startup path).
#

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Determine which compression methods are compiled in, preferring the
# faster hardware-accelerated codecs first for quicker failure feedback.
my @methods = ();
push @methods, 'zstd' if check_pg_config('#define HAVE_LIBZSTD 1');
push @methods, 'lz4'  if check_pg_config('#define HAVE_LIBLZ4 1');
push @methods, 'pglz';

# Test whole-record WAL compression with a specific method.
#
# Creates a primary with the method enabled and a low threshold so that
# every non-trivial WAL record is compressed.  A streaming standby is
# used to verify that compressed WAL is transmitted and decoded correctly.
# Then the primary is stopped immediately (simulating a crash) and
# restarted to verify that startup recovery can decompress WAL records.
# Emit one 256kB compressible logical message at the given threshold and
# report how many bytes of WAL it consumed.
sub wal_used
{
	my ($node, $threshold) = @_;

	my $lsns = $node->safe_psql(
		'postgres', qq{
		SET wal_compression_threshold = $threshold;
		SELECT pg_current_wal_insert_lsn();
		SELECT pg_logical_emit_message(true, 'test 052', repeat('abcd', 65536));
		SELECT pg_current_wal_insert_lsn();
	});

	# Three result rows: the LSN before, the message's own LSN, the LSN after.
	my ($before, $after) = (split /\n/, $lsns)[ 0, 2 ];
	return $node->safe_psql('postgres',
		"SELECT '$after'::pg_lsn - '$before'::pg_lsn");
}

sub test_wal_compression
{
	my ($method) = @_;

	note "testing wal_compression = $method";

	my $primary = PostgreSQL::Test::Cluster->new("primary_$method");
	$primary->init(allows_streaming => 1);

	# Use the minimum threshold so virtually every record gets compressed.
	$primary->append_conf(
		'postgresql.conf',
		"wal_compression = '$method'\n"
		  . "wal_compression_threshold = 32\n");
	$primary->start;

	my $backup_name = "backup_$method";
	$primary->backup($backup_name);

	my $standby = PostgreSQL::Test::Cluster->new("standby_$method");
	$standby->init_from_backup($primary, $backup_name, has_streaming => 1);
	$standby->start;

	# Prove the feature actually engages, rather than silently doing nothing.
	# A large, highly compressible logical message is one record with no
	# full-page images, so the WAL it occupies is decided purely by whole-record
	# compression.  Measure it against the same message with the threshold
	# raised out of reach.
	my $compressed_bytes = wal_used($primary, 32);
	my $plain_bytes = wal_used($primary, 1024 * 1024 * 1024);

	if ($method eq 'pglz')
	{
		# pglz is excluded from whole-record compression on purpose, so the
		# threshold must make no difference at all.
		is($compressed_bytes, $plain_bytes,
			"whole-record compression does not engage for pglz");
	}
	else
	{
		cmp_ok($compressed_bytes, '<', $plain_bytes / 2,
			"whole-record compression shrinks the record ($method): "
			  . "$compressed_bytes vs $plain_bytes bytes");
	}

	my $start_lsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

	# Generate WAL with records that exceed the compression threshold.
	# Each row's WAL record includes 200-byte payload well above 32 bytes.
	$primary->safe_psql(
		'postgres',
		"CREATE TABLE t AS
		 SELECT g, repeat('x', 200) AS d
		 FROM generate_series(1, 100) AS g");

	$primary->wait_for_replay_catchup($standby);

	is( $standby->safe_psql('postgres', 'SELECT count(*) FROM t'),
		'100',
		"compressed WAL replicated via streaming ($method)");

	# Insert another batch that will be recovered after the simulated crash.
	$primary->safe_psql(
		'postgres',
		"INSERT INTO t
		 SELECT g, repeat('y', 200)
		 FROM generate_series(101, 200) AS g");

	# pg_waldump has to walk these records too.  That is the frontend decoding
	# path, which reaches XLogDecompressRecordIfNeeded() without any of the
	# server's state, so it is worth covering separately from recovery.
	my $end_lsn =
	  $primary->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');

	$primary->command_ok(
		[
			'pg_waldump',
			'--quiet',
			'--path' => $primary->data_dir . '/pg_wal',
			'--start' => $start_lsn,
			'--end' => $end_lsn
		],
		"pg_waldump decodes compressed WAL ($method)");

	# Stop without a clean shutdown.  On restart PostgreSQL will replay WAL
	# from the last checkpoint, exercising XLogDecompressRecordIfNeeded for
	# every compressed record generated since that checkpoint.
	$primary->stop('immediate');
	$primary->start;

	is( $primary->safe_psql('postgres', 'SELECT count(*) FROM t'),
		'200',
		"crash recovery replays compressed WAL ($method)");

	$primary->stop;
	$standby->stop;
}

foreach my $method (@methods)
{
	test_wal_compression($method);
}

done_testing();
