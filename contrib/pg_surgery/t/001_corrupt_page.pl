# Copyright (c) 2026, PostgreSQL Global Development Group

# Feed pg_surgery pages that are already corrupt and check that it skips
# them with a NOTICE instead of reading or writing out of bounds.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;

use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION pg_surgery');

# One table whose page header we damage, one whose line pointer we damage.
$node->safe_psql(
	'postgres', q{
	CREATE TABLE t_hdr (a int);
	INSERT INTO t_hdr SELECT generate_series(1, 5);
	CREATE TABLE t_lp (a int);
	INSERT INTO t_lp SELECT generate_series(1, 5);
	CHECKPOINT;
});

my $hdr_path = $node->safe_psql('postgres',
	q{SELECT pg_relation_filepath('t_hdr')});
my $lp_path = $node->safe_psql('postgres',
	q{SELECT pg_relation_filepath('t_lp')});

$node->stop;

# t_hdr: force pd_lower = pd_upper = pd_special = BLCKSZ so the page's max
# offset (derived from pd_lower) is far above MaxHeapTuplesPerPage.
overwrite($hdr_path, 12, pack('S*', 8192, 8192, 8192));

# t_lp: leave the header alone, but point the first line pointer past the
# end of the page.  Item is (lp_off = 32767, lp_flags = LP_NORMAL,
# lp_len = 100) packed into the 32-bit ItemIdData word.
overwrite($lp_path, 24, pack('L', 32767 | (1 << 15) | (100 << 17)));

$node->start;

# A corrupt header must be skipped, not overrun include_this_tid[].
my ($ret, $out, $err) = $node->psql('postgres',
	q{SELECT heap_force_kill('t_hdr'::regclass, ARRAY['(0,1)']::tid[])});
is($ret, 0, 'kill on corrupt-header page returns cleanly');
like($err, qr/because the page header is invalid/,
	'corrupt-header block is skipped with a NOTICE');

# A line pointer outside the page must not be dereferenced by a freeze.
($ret, $out, $err) = $node->psql('postgres',
	q{SELECT heap_force_freeze('t_lp'::regclass, ARRAY['(0,1)']::tid[])});
is($ret, 0, 'freeze on out-of-page line pointer returns cleanly');
like($err, qr/because its line pointer is invalid/,
	'out-of-page line pointer is skipped by freeze');

# A kill does not dereference the tuple, so it still marks that line
# pointer dead rather than skipping it.
($ret, $out, $err) = $node->psql('postgres',
	q{SELECT heap_force_kill('t_lp'::regclass, ARRAY['(0,1)']::tid[])});
is($ret, 0, 'kill on out-of-page line pointer returns cleanly');
unlike($err, qr/because its line pointer is invalid/,
	'kill still processes an out-of-page line pointer');

# The server survived every operation above.
is($node->safe_psql('postgres', 'SELECT 1'), 1, 'server is still up');

$node->stop;

done_testing();

sub overwrite
{
	my ($relpath, $offset, $bytes) = @_;
	my $path = $node->data_dir . '/' . $relpath;

	open(my $fh, '+<', $path) or BAIL_OUT("open $path failed: $!");
	binmode $fh;
	sysseek($fh, $offset, 0) or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, $bytes) or BAIL_OUT("syswrite failed: $!");
	close($fh) or BAIL_OUT("close failed: $!");
}
