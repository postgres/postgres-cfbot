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
# Render a string with its non-printable bytes visible, so that a difference
# which a terminal hides can still be read in the test output.
sub _escape
{
	my ($str) = @_;
	$str =~ s/([^\x20-\x7e])/sprintf('\\x%02x', ord($1))/ge;
	return $str;
}

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
	# Streams are off here: this part measures what whole-record compression
	# does on its own, and streams would compress both sides of that
	# comparison.  They are covered separately below.
	$primary->append_conf(
		'postgresql.conf',
		"wal_compression = '$method'\n"
		  . "wal_compression_threshold = 32\n"
		  . "wal_compression_streams = 0\n");
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

# A record compressed against earlier records of its stream cannot be read on
# its own, so a reader that starts partway through WAL has to rewind far enough
# to rebuild the decompressors.  Check that what it then reads is what a reader
# that started at the beginning sees at the same place.
SKIP:
{
	skip 'zstd not supported by this build', 3
	  unless check_pg_config('#define HAVE_LIBZSTD 1');

	my $node = PostgreSQL::Test::Cluster->new('streams');
	$node->init;
	$node->append_conf(
		'postgresql.conf', qq(
wal_compression = zstd
wal_compression_streams = 8
wal_compression_threshold = 64
wal_keep_size = 1GB
max_wal_size = 1GB
));
	$node->start;

	my $start_lsn = $node->safe_psql('postgres',
		'SELECT pg_current_wal_insert_lsn()');

	# Enough traffic to cross several stream reset boundaries.
	$node->safe_psql(
		'postgres', q{
		CREATE TABLE t (id int, pad text);
		INSERT INTO t SELECT g, repeat('a', 200) FROM generate_series(1, 200000) g;
		UPDATE t SET pad = repeat('b', 200) WHERE id % 3 = 0;
	});
	my $end_lsn =
	  $node->safe_psql('postgres', 'SELECT pg_current_wal_flush_lsn()');

	my $waldir = $node->data_dir . '/pg_wal';
	my ($full, $full_err) = run_command(
		[ 'pg_waldump', '--path' => $waldir,
		  '--start' => $start_lsn, '--end' => $end_lsn ]);
	my @full = split(/\n/, $full);
	ok(@full > 1000, 'the workload produced records to read');

	# Start halfway in, at an LSN that is nobody's record boundary.
	my $mid = $node->safe_psql('postgres',
		"SELECT ('$start_lsn'::pg_lsn + (('$end_lsn'::pg_lsn - '$start_lsn'::pg_lsn) / 2)::bigint)::text"
	);
	my ($part, $part_err) = run_command(
		[ 'pg_waldump', '--path' => $waldir,
		  '--start' => $mid, '--end' => $end_lsn ]);
	my @part = split(/\n/, $part);
	unlike($part_err, qr/error/,
		'reading from an arbitrary LSN reports no error');

	# Whatever it starts with must appear in the full dump, and everything
	# from there on must match it line for line.  Trailing whitespace is
	# normalised away: how a line ends is the platform's business, not this
	# test's.
	my ($first) = $part[0] =~ /lsn: ([0-9A-F]+\/[0-9A-F]+),/;
	my ($at) = grep { $full[$_] =~ /lsn: \Q$first\E,/ } 0 .. $#full;
	my @tail = defined $at ? @full[ $at .. $#full ] : ();
	s/\s+\z// for @part, @tail;

	my $same = is_deeply(\@part, \@tail,
		'records read from an arbitrary LSN match a full read');

	# On failure, say what actually differs: the strings print the same when
	# they differ only in bytes a terminal does not show.
	if (!$same)
	{
		diag(sprintf('read from %s: %d lines, tail of full read: %d lines',
				$mid, scalar(@part), scalar(@tail)));
		for my $i (0 .. $#part)
		{
			next if defined $tail[$i] and $part[$i] eq $tail[$i];
			diag(sprintf("first difference at line %d\n  got: %s\n  exp: %s",
					$i,
					_escape($part[$i]),
					defined $tail[$i] ? _escape($tail[$i]) : '(missing)'));
			last;
		}
	}

	# pg_walinspect reads from an LSN its caller picks, so it has to rewind
	# the same way pg_waldump does.
  SKIP:
	{
		skip 'pg_walinspect not installed', 1
		  unless $node->check_extension('pg_walinspect');

		$node->safe_psql('postgres', 'CREATE EXTENSION pg_walinspect');
		my $differing = $node->safe_psql(
			'postgres', qq{
			WITH f AS (SELECT start_lsn, record_type, record_length
			             FROM pg_get_wal_records_info('$start_lsn', '$end_lsn')
			            WHERE start_lsn >= '$mid'::pg_lsn),
			     t AS (SELECT start_lsn, record_type, record_length
			             FROM pg_get_wal_records_info('$mid', '$end_lsn'))
			SELECT (SELECT count(*) FROM (SELECT * FROM f EXCEPT ALL SELECT * FROM t) a)
			     + (SELECT count(*) FROM (SELECT * FROM t EXCEPT ALL SELECT * FROM f) b)
		});
		is($differing, '0',
			'pg_walinspect from an arbitrary LSN matches a full read');
	}

	$node->stop;
}

done_testing();
