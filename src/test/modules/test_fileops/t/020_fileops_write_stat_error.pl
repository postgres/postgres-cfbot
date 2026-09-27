# Copyright (c) 2026, PostgreSQL Global Development Group

# WRITE UNDO apply must not treat a stat *error* as "nothing to undo".
#
# A crash loser's FILEOPS WRITE overwrote bytes in a file; the before-image
# is in its UNDO chain.  If, during crash recovery, lstat() on the target
# fails with something other than ENOENT (e.g. EACCES/EIO), the WRITE inverse
# must fail like every other subtype (-> UNDO_APPLY_ERROR) so the loser is
# handed to the logical revert worker and retried, not silently dropped with
# its aborted bytes left on disk.
#
# Before the fix, the WRITE case collapsed lstat()'s 0 (ENOENT) and -1
# (error) into "success": the chain was recorded as fully reverted and never
# retried, so the ATM stayed empty and the overwritten bytes were lost.
#
# The stat error is forced deterministically with the
# "fileops-undo-write-lstat-error" injection point, which must be attached
# before the crash it affects (the startup process applies the inverse), so
# test_fileops attaches it from its shmem_startup_hook under a GUC.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_write_stat_error');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
autovacuum = off
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
shared_preload_libraries = 'test_fileops'
test_fileops.write_lstat_error = on
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $path = $node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('w.dat')");

# Leave the WRITE in flight, all of its UNDO written, then crash.  The file
# stays present and readable; recovery's lstat() is forced to report an error
# only through the injection point.
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_write('$path', 0, 'ROLLBACKME')");
my $xid = $bg->query_safe('SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

my $offset = -s $node->logfile;
$node->stop('immediate');
$bg->{run}->finish;
$node->start;

my $log = slurp_file($node->logfile, $offset);
like(
	$log,
	qr/UNDO recovery: rolling back transaction $xid\b/,
	'the FILEOPS WRITE transaction was found as a loser');

# The stat error must have failed the WRITE inverse, so the loser is now
# deferred to the logical revert worker rather than recorded as reverted.
is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
	't',
	'a stat error defers the WRITE loser instead of dropping it');

# Turn the error off and let the worker finish: the before-image is restored
# and the ATM drains.
$node->adjust_conf('postgresql.conf', 'test_fileops.write_lstat_error', 'off');
$node->adjust_conf('postgresql.conf', 'max_logical_revert_workers', '2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	'the worker drained the deferred WRITE loser');
is(substr(slurp_file($path), 0, 10),
	'XXXXXXXXXX',
	'the overwritten bytes were restored');

$node->stop;
done_testing();
