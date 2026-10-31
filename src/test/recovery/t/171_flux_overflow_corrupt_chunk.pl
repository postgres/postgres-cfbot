
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX-native overflow: a reader must never trust a length read off an
# overflow chunk/directory page.  Three fields the overflow code reads from a
# page drive a memcpy or a palloc sized by that same page byte:
#
#   (a) a DATA chunk's or_data_len     -- reassembled into the result buffer;
#   (b) the ROOT directory's od_nentries -- sizes the entry palloc + memcpy;
#   (c) an INDIRECT directory's kind/od_nentries -- the VACUUM reachability
#       walk trusts it to decide which chunks are live.
#
# Each is corrupted on disk (amcheck/t/001_verify_heapam.pl style: stop the
# server, overwrite bytes in the relation file, restart) and then exercised:
#
#   (a)/(b) a SELECT of the value must raise a "data corrupted"-class ERROR,
#           never crash (on the ASAN build a missing bound is a
#           heap-buffer-overflow / over-read, i.e. a hard abort);
#   (c) VACUUM must fail safe -- emit a WARNING, free 0 chunks, and leave every
#       intact wide value readable -- never free a live chunk on uncertainty.
#
# Overflow records are located by scanning the relation file for the record
# magic "ROVC" (FLUX_OVERFLOW_RECORD_MAGIC) and classifying each by its
# on-disk kind (or_flags & FLUX_OVCHUNK_KIND_MASK).  The on-disk layout of
# FluxOverflowRecordHeader is:
#   +0  uint32 or_magic ("ROVC")
#   +4  uint32 or_data_len
#   +8  uint32 or_xid
#   +12 uint16 or_pad
#   +14 uint16 or_flags  (kind in the low 2 bits: 0 DATA, 1 ROOT, 2 INDIRECT)
# followed at +16 (FLUX_OVERFLOW_RECORD_OVERHEAD) by the payload; for a ROOT or
# INDIRECT record the payload is a FluxOverflowDir whose od_nentries is at
# payload +8, i.e. record +24.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $OV_MAGIC    = 0x524F5643;    # "ROVC"
my $KIND_MASK   = 0x0003;
my $KIND_DATA   = 0x0000;
my $KIND_ROOT   = 0x0001;
my $KIND_INDIR  = 0x0002;
my $HDR_OVERHEAD = 16;           # FLUX_OVERFLOW_RECORD_OVERHEAD (MAXALIGN(16))
my $DIR_NENTRIES_OFF = 8;        # od_nentries within FluxOverflowDir

# Resolve a relation's on-disk segment list.  Must be called while the server
# is up (it reads pg_relation_filepath); the returned list is then used by the
# file scanner/poker after the server is stopped.
sub segment_list
{
	my ($node, $relname) = @_;
	my $pgdata = $node->data_dir;
	my $rel = $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$relname')));
	my @segs = ("$pgdata/$rel");
	# Every segment: a build with small segments (--with-segsize-blocks) spreads
	# one wide value over hundreds of them.
	for (my $n = 1; -f "$pgdata/$rel.$n"; $n++) { push @segs, "$pgdata/$rel.$n"; }
	return @segs;
}

# Scan the given data-file segment(s) for every overflow record and return a
# list of { pos => flattened file byte offset of the header, len => or_data_len,
# kind => kind }.  Works with the server down.
sub scan_overflow_records
{
	my (@segs) = @_;

	my @recs;
	my $base = 0;
	for my $seg (@segs)
	{
		open(my $fh, '<', $seg) or die "open $seg: $!";
		binmode $fh;
		local $/;
		my $data = <$fh>;
		close($fh);
		my $len = length($data);
		my $p = 0;
		while ($p + $HDR_OVERHEAD <= $len)
		{
			my $magic = unpack('L', substr($data, $p, 4));
			if ($magic == $OV_MAGIC)
			{
				my $dlen  = unpack('L', substr($data, $p + 4, 4));
				my $flags = unpack('S', substr($data, $p + 14, 2));
				push @recs,
				  {
					pos  => $base + $p,
					len  => $dlen,
					kind => ($flags & $KIND_MASK),
				  };
			}
			$p += 4;    # records are MAXALIGNed; step 4 is enough and cheap
		}
		$base += $len;
	}
	return @recs;
}

# Overwrite a 4-byte little-endian uint32 at a flattened file offset across the
# same @segs list scan_overflow_records walked.  Works with the server down.
sub poke_u32
{
	my ($pos, $val, @segs) = @_;

	my $base = 0;
	for my $seg (@segs)
	{
		my $sz = -s $seg;
		if ($pos >= $base && $pos < $base + $sz)
		{
			open(my $fh, '+<', $seg) or die "open $seg: $!";
			binmode $fh;
			seek($fh, $pos - $base, 0);
			print $fh pack('L', $val);
			close($fh);
			return;
		}
		$base += $sz;
	}
	die "offset $pos past end of relation";
}

# Read a little-endian u32 at a flattened offset into the segments.
sub read_u32
{
	my ($pos, @segs) = @_;

	my $base = 0;
	for my $seg (@segs)
	{
		my $sz = -s $seg;
		if ($pos >= $base && $pos < $base + $sz)
		{
			open(my $fh, '<', $seg) or die "open $seg: $!";
			binmode $fh;
			seek($fh, $pos - $base, 0);
			read($fh, my $buf, 4);
			close($fh);
			return unpack('L', $buf);
		}
		$base += $sz;
	}
	die "offset $pos past end of relation";
}

sub big { my ($seed) = @_; return "repeat(md5($seed)::text, 320)" }

# ---------------------------------------------------------------------------
# (a) inflate a DATA chunk's or_data_len; (b) inflate the ROOT's od_nentries.
# Both are read on a plain SELECT of the value, so one small single-node
# cluster exercises both.  A ~10kB incompressible EXTERNAL value spans several
# DATA chunks with a flat (levels=0) ROOT directory.
# ---------------------------------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('ovcorrupt');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', "autovacuum = off\n");
$node->start;

$node->safe_psql(
	'postgres', qq{
CREATE TABLE ov (id int PRIMARY KEY, big text) USING flux;
ALTER TABLE ov ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO ov VALUES (1, @{[big('1::text')]});
});

# Sanity: the value reads back before corruption.
is( $node->safe_psql('postgres',
		"SELECT big = @{[big('1::text')]} FROM ov WHERE id = 1"),
	't', 'value reads back intact before corruption');

my @ov_segs = segment_list($node, 'ov');

# --- (a) inflate a DATA chunk's or_data_len past its item.
$node->stop;
{
	my @recs = scan_overflow_records(@ov_segs);
	my @data = grep { $_->{kind} == $KIND_DATA } @recs;
	die "no DATA chunk found" unless @data;
	# Inflate the first DATA chunk's or_data_len (header +4) well past
	# FLUX_OVERFLOW_MAX_CHUNK_SIZE so reassembly would run off the result.
	poke_u32($data[0]->{pos} + 4, 0x00100000, @ov_segs);
}
$node->start;

my ($rc, $out, $err) =
  $node->psql('postgres', "SELECT length(big) FROM ov WHERE id = 1");
isnt($rc, 0, '(a) inflated DATA or_data_len: SELECT fails');
like($err, qr/data_corrupted|corrupt|overflow/i,
	'(a) error is a data-corruption-class error, not a crash');
ok( !$node->log_contains(
		'TRAP|PANIC|runtime error|AddressSanitizer|heap-buffer-overflow'),
	'(a) no crash / ASAN abort in the server log');

# --- (b) inflate the ROOT directory's od_nentries.
$node->stop;
{
	my @recs = scan_overflow_records(@ov_segs);
	my @root = grep { $_->{kind} == $KIND_ROOT } @recs;
	die "no ROOT chunk found" unless @root;
	# od_nentries lives at record +HDR_OVERHEAD +DIR_NENTRIES_OFF.
	poke_u32($root[0]->{pos} + $HDR_OVERHEAD + $DIR_NENTRIES_OFF,
		0x00100000, @ov_segs);
}
$node->start;

($rc, $out, $err) =
  $node->psql('postgres', "SELECT length(big) FROM ov WHERE id = 1");
isnt($rc, 0, '(b) inflated ROOT od_nentries: SELECT fails');
like($err, qr/data_corrupted|corrupt|overflow/i,
	'(b) error is a data-corruption-class error, not a crash');
ok( !$node->log_contains(
		'TRAP|PANIC|runtime error|AddressSanitizer|heap-buffer-overflow'),
	'(b) no crash / ASAN abort in the server log');

# --- (b2) point a ROOT directory entry past the end of its chunk's page.
# PageGetItemId() does not check the offset, so a reader that trusts it reads
# beyond the line-pointer array.  A second value, so (b)'s damage is not hit.
$node->safe_psql('postgres',
	"INSERT INTO ov VALUES (2, @{[big('2::text')]})");
$node->stop;
{
	my @recs = scan_overflow_records(@ov_segs);
	# The new value's ROOT is the one (b) did not inflate.  Records come back
	# in file order, and the new chunks may sit on an earlier page.
	my @root = grep {
		$_->{kind} == $KIND_ROOT
		  && read_u32($_->{pos} + $HDR_OVERHEAD + $DIR_NENTRIES_OFF, @ov_segs)
		  != 0x00100000
	} @recs;
	die "no intact ROOT chunk found" unless @root == 1;
	# Entry 0's offset: record +HDR_OVERHEAD +FLUX_OVERFLOW_DIR_HDR (16)
	# +block (4).  Rewrite the 4 bytes offset+pad, pad kept zero.
	poke_u32($root[0]->{pos} + $HDR_OVERHEAD + 16 + 4, 0x0000FFFF, @ov_segs);
}
$node->start;

($rc, $out, $err) =
  $node->psql('postgres', "SELECT length(big) FROM ov WHERE id = 2");
isnt($rc, 0, '(b2) ROOT entry offset past the page: SELECT fails');
# Checked against the page's line-pointer count, not read off the end of the
# line-pointer array (which lies inside the page, so no sanitizer notices).
like($err, qr/is past the end of its page/,
	'(b2) the entry offset is checked before its line pointer is read');
ok( !$node->log_contains(
		'TRAP|PANIC|runtime error|AddressSanitizer|heap-buffer-overflow'),
	'(b2) no crash / ASAN abort in the server log');

# --- (b3) inflate the ROOT's od_total_length to just under 4 GB.  The reader
# sizes its entry array and result from it, and must refuse it as corruption,
# then still serve a later query (no leaked buffer lock or pin).
$node->safe_psql('postgres',
	"INSERT INTO ov VALUES (3, @{[big('3::text')]})");
$node->stop;
{
	my @recs = scan_overflow_records(@ov_segs);
	my @root = grep {
		$_->{kind} == $KIND_ROOT
		  && read_u32($_->{pos} + $HDR_OVERHEAD + $DIR_NENTRIES_OFF, @ov_segs)
		  != 0x00100000
		  && read_u32($_->{pos} + $HDR_OVERHEAD + 16 + 4, @ov_segs) != 0x0000FFFF
	} @recs;
	die "no intact ROOT chunk found" unless @root == 1;
	# od_total_length is the first word of the directory.
	poke_u32($root[0]->{pos} + $HDR_OVERHEAD, 0xFFFFFFF0, @ov_segs);
}
$node->start;

($rc, $out, $err) =
  $node->psql('postgres', "SELECT length(big) FROM ov WHERE id = 3");
isnt($rc, 0, '(b3) inflated ROOT od_total_length: SELECT fails');
like($err, qr/value length \d+ is too large/,
	'(b3) the value length is refused as corruption');
is($node->safe_psql('postgres', 'SELECT 1'),
	'1', '(b3) the server still serves queries');
ok( !$node->log_contains(
		'TRAP|PANIC|runtime error|AddressSanitizer|heap-buffer-overflow|buffer refcount leak|lock .* is not held'),
	'(b3) no crash, sanitizer report or leaked buffer in the server log');

$node->stop;

# ---------------------------------------------------------------------------
# (c) corrupt an INDIRECT chunk on a value large enough (>~8 MB) to have one,
# then VACUUM.  The reachability walk reads the INDIRECT directory; a corrupt
# od_nentries must make VACUUM fail safe -- WARNING, 0 chunks freed, and the
# other intact wide value still reads by md5 -- never free a live chunk.
# ---------------------------------------------------------------------------
my $vnode = PostgreSQL::Test::Cluster->new('ovindir');
$vnode->init(no_data_checksums => 1);
$vnode->append_conf('postgresql.conf', "autovacuum = off\n");
$vnode->start;

# ~12 MB incompressible EXTERNAL value forces a 2-level directory (one
# INDIRECT level); a second, smaller intact wide value must survive the VACUUM.
$vnode->safe_psql(
	'postgres', q{
CREATE TABLE ovi (id int PRIMARY KEY, big text) USING flux;
ALTER TABLE ovi ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO ovi VALUES (1, (SELECT string_agg(md5(g::text), '')
                            FROM generate_series(1, 400000) g));
INSERT INTO ovi VALUES (2, repeat(md5('keep'::text)::text, 320));
});

my @ovi_segs = segment_list($vnode, 'ovi');
my $keep_md5 = $vnode->safe_psql('postgres',
	"SELECT md5(repeat(md5('keep'::text)::text, 320))");

$vnode->stop;
my $chunks_before = scalar scan_overflow_records(@ovi_segs);
{
	my @recs = scan_overflow_records(@ovi_segs);
	my @indir = grep { $_->{kind} == $KIND_INDIR } @recs;
	die "no INDIRECT chunk found (value not big enough?)" unless @indir;
	# Corrupt the INDIRECT directory's od_nentries to a huge value: the walk
	# must detect it and abandon reclaim rather than over-read / mis-free.
	poke_u32($indir[0]->{pos} + $HDR_OVERHEAD + $DIR_NENTRIES_OFF,
		0x00100000, @ovi_segs);
}
$vnode->start;

# VACUUM must not crash or ERROR; it must warn and reclaim nothing.
($rc, $out, $err) = $vnode->psql('postgres', 'VACUUM ovi');
is($rc, 0, '(c) VACUUM over a corrupt INDIRECT dir does not ERROR out');
like($err, qr/skipping orphan reclaim|WARNING/i,
	'(c) VACUUM emits a fail-safe WARNING');
ok( !$vnode->log_contains(
		'TRAP|PANIC|runtime error|AddressSanitizer|heap-buffer-overflow'),
	'(c) no crash / ASAN abort during VACUUM');

$vnode->stop;
my $chunks_after = scalar scan_overflow_records(@ovi_segs);
is($chunks_after, $chunks_before,
	'(c) VACUUM freed 0 chunks on uncertainty');
$vnode->start;

# The intact wide value (row 2) must still read back by md5; row 1's value is
# corrupt on disk, but reading row 2 must not crash and must match.
is( $vnode->safe_psql('postgres', "SELECT md5(big) FROM ovi WHERE id = 2"),
	$keep_md5,
	'(c) the intact wide value still reads back by md5 after the failed VACUUM');

$vnode->stop;
done_testing();
