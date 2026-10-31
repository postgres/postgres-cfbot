
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX in-place UPDATE lost update, reproduced through ordinary SQL with many
# concurrent single-statement transactions.
#
# The bug (seen first by HammerDB TPROC-C, 50 WH / 64 VU, FLUX only): committed
# PAYMENT increments of a hot warehouse row (UPDATE warehouse SET w_ytd =
# w_ytd + x) were silently lost -- w_ytd < sum(d_ytd) on 9 of 50 warehouses,
# 45,362 increments lost in total; heap on the same binary lost none.
#
# Two lost-update hazards on FLUX's in-place UPDATE path, both exercised here:
#
#   NARROW (no overflow).  The in-place slow path's write-write serialization
#   re-checks the identity of the on-page version it decided from
#   (decided_verptr/decided_xmin, flux_same_version), captured for every
#   in-place UPDATE of an indexed table -- including the non-key TPC-C
#   "w_ytd = w_ytd + x" shape -- so a writer that committed while we waited is
#   never overwritten with a tuple formed from the pre-conflict values.
#
#   WIDE (FLUX-native overflow).  The UPDATE writes the new value's overflow
#   chunks with the page unlocked and must re-check the row version when it
#   relocks (flux_recheck_version_after_relock), as force-shrink does.
#
# This test drives $CLIENTS single-statement UPDATE transactions against a
# small FLUX table of TPC-C-warehouse-shaped counter rows.  Each transaction
# does `UPDATE w SET ytd = ytd + 1, pad = <new text> WHERE id = random(1..N)`.
# The pad rewrite changes the tuple's size every time, so every UPDATE takes
# the slow path (not the same-size CAS fast path).  Because exactly one row's
# ytd rises by one per committed transaction, the global invariant is:
#
#       SUM(ytd) over all rows  ==  number of committed UPDATE transactions
#
# pgbench reports the committed count ("number of transactions actually
# processed").  A lost update shows up as SUM(ytd) < committed.  A heap twin of
# the identical workload is the control: it must show zero shortfall.
#
# Two FLUX variants run by default:
#   narrow -- pad is a short varchar; the tuple changes size in place (no
#             overflow): exercises the version re-check.
#   wide   -- pad is an ~8 KB incompressible EXTERNAL varchar; the UPDATE writes
#             FLUX-native overflow chunks with the page unlocked (re-checked
#             after relock).
#
# Without either guard FLUX shows SUM(ytd) < committed.  Hard runtime cap
# < 3 min (two short FLUX drives + their heap controls).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run qw(run);

my $CLIENTS       = $ENV{FLUX_LU_CLIENTS} || 64;
my $THREADS       = 8;
my $DRIVE_SECONDS = $ENV{FLUX_LU_SECONDS} || 20;
my $ROWS          = $ENV{FLUX_LU_ROWS} || 8;     # few hot rows -> heavy contention

# ---------------------------------------------------------------------------
# Cluster.  cassert debug build; autovacuum off so pages do not move under us.
# ---------------------------------------------------------------------------
my $node = PostgreSQL::Test::Cluster->new('flux_inplace_lost_update');
$node->init;
$node->append_conf(
	'postgresql.conf', qq{
autovacuum = off
max_connections = 100
fsync = off
});
$node->start;

my $DB = 'postgres';

# ---------------------------------------------------------------------------
# Create a FLUX counter table (+ a heap twin for the narrow control) of the
# given pad width, packed, fillfactor 100.
# ---------------------------------------------------------------------------
sub make_tables
{
	my ($suffix, $wide) = @_;
	my $flux = "wflux_$suffix";
	my $heap = "wheap_$suffix";
	my $pad_type = $wide ? 'varchar(16000)' : 'varchar(200)';
	my $pad_ddl  = $wide ? "ALTER TABLE $flux ALTER COLUMN pad SET STORAGE EXTERNAL;" : "";

	$node->safe_psql(
		$DB, qq{
CREATE TABLE $flux (
    id    int PRIMARY KEY,
    name  varchar(16) NOT NULL,
    city  varchar(20) NOT NULL,
    state varchar(2)  NOT NULL,
    ytd   numeric(14,2) NOT NULL,
    pad   $pad_type NOT NULL
) USING flux WITH (fillfactor = 100);
$pad_ddl
INSERT INTO $flux
SELECT g, left(md5(g::text),16), left(md5((g+1)::text),20), 'AA', 0, 'x'
FROM generate_series(1, $ROWS) g;

CREATE TABLE $heap (LIKE $flux INCLUDING ALL) USING heap;
INSERT INTO $heap
SELECT g, left(md5(g::text),16), left(md5((g+1)::text),20), 'AA', 0, 'x'
FROM generate_series(1, $ROWS) g;
});
	return ($flux, $heap);
}

# pgbench script: one single-statement UPDATE per transaction, ytd += 1 plus a
# fresh pad value (narrow: 32..192 B varying so size changes; wide: ~8 KB
# incompressible EXTERNAL so overflow chunks are written).
sub make_script
{
	my ($tbl, $wide, $file) = @_;
	my $pad_expr = $wide
	  ? "repeat(md5(random()::text), 256)"
	  : "repeat(md5(random()::text), 1 + (random()*5)::int)";
	append_to_file(
		$file, qq{
\\set id random(1, $ROWS)
UPDATE $tbl SET ytd = ytd + 1, pad = $pad_expr WHERE id = :id;
});
	return $file;
}

# Drive one table for $DRIVE_SECONDS, return (committed_count, sum_ytd).
# pgbench's committed count is the independent oracle.
sub drive_counted
{
	my ($tbl, $script) = @_;
	my ($stdout, $stderr) = ('', '');
	my $okrun = run(
		[
			'pgbench', '--no-vacuum',
			'--client' => $CLIENTS,
			'--jobs'   => $THREADS,
			'--time'   => $DRIVE_SECONDS,
			'--file'   => $script,
			$node->connstr($DB),
		],
		'>'  => \$stdout,
		'2>' => \$stderr);
	note "pgbench($tbl) stdout:\n$stdout";
	note "pgbench($tbl) stderr:\n$stderr" if $stderr ne '';
	ok($okrun, "pgbench drove $tbl successfully");

	my ($processed) = $stdout =~ /number of transactions actually processed:\s*(\d+)/;
	my ($failed) = $stdout =~ /number of failed transactions:\s*(\d+)/;
	$failed = 0 unless defined $failed;
	die "could not parse pgbench output for $tbl" unless defined $processed;
	my $committed = $processed - $failed;

	my $sum = $node->safe_psql($DB, "SELECT sum(ytd)::bigint FROM $tbl");
	note "$tbl: committed=$committed sum(ytd)=$sum shortfall="
	  . ($committed - $sum);
	return ($committed, $sum + 0);
}

# One variant: build tables, run the heap control + the FLUX assertion.
sub run_variant
{
	my ($label, $wide) = @_;
	my ($flux, $heap) = make_tables($label, $wide);
	my $fscript = make_script($flux, $wide,
		$PostgreSQL::Test::Utils::tmp_check . "/165_${label}_flux.sql");
	my $hscript = make_script($heap, $wide,
		$PostgreSQL::Test::Utils::tmp_check . "/165_${label}_heap.sql");

	my ($hc, $hs) = drive_counted($heap, $hscript);
	is($hs, $hc, "heap control ($label): sum(ytd) == committed (no lost update)");

	my ($fc, $fs) = drive_counted($flux, $fscript);
	is($fs, $fc,
		"flux in-place UPDATE ($label): sum(ytd) == committed (no lost update)")
	  or diag("FLUX ($label) lost " . ($fc - $fs)
		  . " committed increments out of $fc");
}

run_variant('narrow', 0);
run_variant('wide',   1);

$node->stop;
done_testing();
