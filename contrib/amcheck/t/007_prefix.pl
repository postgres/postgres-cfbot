# Copyright (c) 2026, PostgreSQL Global Development Group

# Check prefix metadata before amcheck decodes tuples or recounts their sizes.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('prefix');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION amcheck;
	CREATE TABLE prefix_data (u text COLLATE "C");
	INSERT INTO prefix_data
	  SELECT repeat('prefix/', 12) || lpad(g::text, 4, '0')
	  FROM generate_series(1, 20) g;
	CREATE TABLE prefix_posting AS
	  SELECT u FROM prefix_data CROSS JOIN generate_series(1, 4);
));
my $blksize = $node->safe_psql('postgres', 'SHOW block_size');
my $align = $node->safe_psql('postgres',
	'SELECT max_data_alignment FROM pg_control_init()');
my $little_endian = pack('L', 1) eq "\x01\x00\x00\x00";

# ItemIdData is a native-endian bitfield; tuple metadata uses native uint16.
sub itemid
{
	my ($page, $offnum) = @_;
	my $lp = unpack('L', substr($page, 24 + 4 * ($offnum - 1), 4));
	return $little_endian
	  ? ($lp & 0x7fff, $lp >> 17)
	  : ($lp >> 17, $lp & 0x7fff);
}

sub put_short
{
	my ($page, $offset, $value) = @_;
	substr($$page, $offset, 2) = pack('S', $value);
	return;
}

# Each case changes one field in its own index.  Include a later member so a
# comparison with the next item cannot decode it before it has been checked.
my @cases = (
	[
		'group', 0,
		sub { my ($p, $d, $m) = @_; put_short($p, $m + 8, 65535); }
	],
	[
		'shared length',
		0, sub { my ($p, $d, $m) = @_; put_short($p, $m + 10, 65535); }
	],
	[
		'original size',
		0, sub { my ($p, $d, $m) = @_; put_short($p, $m + 12, 0); }
	],
	[
		'residual extent',
		0, sub { my ($p, $d, $m) = @_; put_short($p, $m + 10, 0); }
	],
	[
		'directory group count',
		0, sub { my ($p, $d) = @_; put_short($p, $d + 8, 65535); }
	],
	[
		'anchor offset',
		0, sub { my ($p, $d) = @_; put_short($p, $d + 14, 65535); }
	],
	[
		'anchor size',
		0,
		sub {
			my ($p, $d) = @_;
			my $a = unpack('S', substr($$p, $d + 14, 2));
			put_short($p, $d + $a, 65535);
		}
	],
	[
		'anchor tuple size',
		0,
		sub {
			my ($p, $d) = @_;
			my $a = unpack('S', substr($$p, $d + 14, 2));
			put_short($p, $d + $a + $align + 6, 0);
		}
	],
	[
		'decoded size cache',
		0, sub { my ($p, $d) = @_; put_short($p, $d + 10, 0); }
	],
	[
		'short code header',
		0,
		sub {
			my ($p, $d, $m) = @_;
			my $lp = unpack('L', substr($$p, 32, 4));
			$lp =
			  $little_endian
			  ? (($lp & 0x1ffff) | (8 << 17))
			  : (($lp & 0xffff8000) | 8);
			substr($$p, 32, 4) = pack('L', $lp);
			put_short($p, $m + 6, 8);
		}
	],
	[
		'posting offset',
		1,
		sub {
			my ($p, $d, $m) = @_;
			put_short($p, $m, 0);
			put_short($p, $m + 2, 65535);
		}
	],
	[
		'posting count',
		1,
		sub {
			my ($p, $d, $m) = @_;
			put_short($p, $m + 4, 0x2000 | 0x0fff);
		}
	],
	[
		'pivot member',
		0,
		sub {
			my ($p) = @_;
			my $maxoff = (unpack('S', substr($$p, 12, 2)) - 24) / 4;
			my ($m) = itemid($$p, 2 + int(($maxoff - 1) / 2));
			my $info = unpack('S', substr($$p, $m + 6, 2));
			put_short($p, $m + 6, $info | 0x2000);
		}
	],
	[
		'member line pointer',
		0,
		sub {
			my ($p) = @_;
			my $maxoff = (unpack('S', substr($$p, 12, 2)) - 24) / 4;
			my $lp_off = 24 + 4 * (1 + int(($maxoff - 1) / 2));
			my $lp = unpack('L', substr($$p, $lp_off, 4));
			$lp = $little_endian
			  ? (($lp & 0xffff8000) | 32760)
			  : (($lp & 0x1ffff) | (32760 << 17));
			substr($$p, $lp_off, 4) = pack('L', $lp);
		}
	],);

my @paths;
for my $i (0 .. $#cases)
{
	my $table = $cases[$i][1] ? 'prefix_posting' : 'prefix_data';
	$node->safe_psql('postgres',
		"CREATE TABLE prefix_case_$i AS TABLE $table;
		 CREATE INDEX prefix_idx_$i ON prefix_case_$i (u)
		 WITH (prefix_compression = on)"
	);
	is( $node->safe_psql(
			'postgres',
			"SELECT bt_index_parent_check('prefix_idx_$i', true, true)"),
		'',
		"$cases[$i][0]: clean index passes");
	push @paths,
	  $node->data_dir . '/'
	  . $node->safe_psql('postgres',
		"SELECT pg_relation_filepath('prefix_idx_$i')");
}

# Stop before opening relation files, avoiding stale buffers and background
# writes.  Checksums are disabled so the errors come from prefix validation.
$node->stop;
for my $i (0 .. $#cases)
{
	open(my $fh, '+<', $paths[$i]) or BAIL_OUT("open: $!");
	binmode $fh;
	defined(sysseek($fh, $blksize, 0)) or BAIL_OUT("seek: $!");
	my $page;
	sysread($fh, $page, $blksize) == $blksize or BAIL_OUT("read: $!");
	my $special = unpack('S', substr($page, 16, 2));
	my $flags = unpack('S', substr($page, $special + 12, 2));
	($flags & 0x201) == 0x201 or BAIL_OUT('fixture is not a coded leaf');
	unpack('L', substr($page, $special + 4, 4)) == 0
	  or BAIL_OUT('fixture leaf is not rightmost');
	my ($directory) = itemid($page, 1);
	my ($member, $membersz) = itemid($page, 3);
	my $info = unpack('S', substr($page, $member + 6, 2));
	($info & 0x1fff) == $membersz or BAIL_OUT('fixture tuple size mismatch');

	if ($cases[$i][1])
	{
		($info & 0x2000)
		  && (unpack('S', substr($page, $member + 4, 2)) & 0x2000)
		  or BAIL_OUT('fixture member is not a posting tuple');
	}
	$cases[$i][2]->(\$page, $directory, $member);
	defined(sysseek($fh, $blksize, 0)) or BAIL_OUT("seek: $!");
	syswrite($fh, $page) == $blksize or BAIL_OUT("write: $!");
	close($fh) or BAIL_OUT("close: $!");
}
$node->start;

for my $i (0 .. $#cases)
{
	for my $check ('bt_index_check', 'bt_index_parent_check')
	{
		my ($ret, $out, $err) = $node->psql('postgres',
			"\\set VERBOSITY verbose\nSELECT $check('prefix_idx_$i');");
		is($ret, 3, "$cases[$i][0]: $check reports an error");
		like($err, qr/ERROR:\s+XX002:.*prefix/i,
			"$cases[$i][0]: prefix corruption diagnostic");
		unlike(
			$err,
			qr/server closed the connection|terminated by signal/i,
			"$cases[$i][0]: no backend crash");
	}
}

# Each table has just one index.  Exercise the shared decoder without the
# whole-page validation performed by amcheck.  The new header cases damage
# the first binary-search midpoint, before any member can be decoded.
my $scan_settings = q(
	SET enable_seqscan = off;
	SET enable_bitmapscan = off;
	SET enable_indexonlyscan = off;
);
for my $i (0 .. $#cases)
{
	next unless $cases[$i][0] =~
	  /^(group|shared length|short code header|posting offset|posting count|pivot member|member line pointer)$/;
	my $query = "SELECT u FROM prefix_case_$i
		WHERE u >= repeat('prefix/', 12) || '0001'";
	my $plan = $node->safe_psql('postgres',
		"$scan_settings EXPLAIN (COSTS OFF) $query");
	like($plan, qr/Index Scan using prefix_idx_$i on prefix_case_$i\b/,
		"$cases[$i][0]: direct index scan plan");
	note($plan);
	my ($ret, $out, $err) = $node->psql('postgres',
		"\\set VERBOSITY verbose\n$scan_settings $query");
	is($ret, 3, "$cases[$i][0]: direct scan reports an error");
	like($err, qr/ERROR:\s+XX002:.*prefix/i,
		"$cases[$i][0]: direct scan prefix diagnostic");
	unlike($err, qr/server closed the connection|terminated by signal/i,
		"$cases[$i][0]: direct scan does not crash");
}

is($node->safe_psql('postgres', 'SELECT 1'),
	'1', 'node remains usable after corruption checks');
$node->stop;
done_testing();
