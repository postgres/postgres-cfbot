# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# A deferred commit-time file operation must survive a crash right after
# its transaction's commit record: the commit is durable, so the deletion
# or rmdir it implies must happen too, on restart if not before.  A rename
# is immediate and logged before it is made, so it survives the crash too.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_commit_crash');
$node->init;
$node->append_conf(
	'postgresql.conf', q{
shared_preload_libraries = 'injection_points'
wal_level = replica
});
$node->start;
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
my $datadir = $node->data_dir;

# Commit $sql, crash right after the commit record, restart.
sub commit_and_crash
{
	my ($sql) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('fileops-commit-before-pending-ops', 'wait')"
	);
	my $bg = $node->background_psql('postgres', on_error_stop => 0);
	$bg->query_until(qr/start/, "\\echo start\n$sql\n");
	$node->wait_for_event('client backend',
		'fileops-commit-before-pending-ops');
	$node->stop('immediate');
	$bg->{run}->finish;
	$node->start;
}

$node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('del.dat')");
$node->safe_psql('postgres',
	"SELECT test_fileops_create_tempfile('ren.dat')");
$node->safe_psql('postgres', "SELECT test_fileops_mkdir('rmd', 448)");
commit_and_crash(
	q{BEGIN;
SELECT test_fileops_delete('del.dat');
SELECT test_fileops_rename('ren.dat', 'ren2.dat');
SELECT test_fileops_rmdir('rmd');
COMMIT;});
ok( !-e "$datadir/ren.dat" && -e "$datadir/ren2.dat",
	'committed FileOpsRename happened despite the crash');
TODO:
{
	# The deferred ops are WAL-logged only when executed, after the commit
	# record, so this crash loses them.  Their intent must be durable before
	# the commit record and executed by commit redo.
	local $TODO =
	  'commit-time FILEOPS ops are logged after the commit record';
	ok(!-e "$datadir/del.dat",
		'committed FileOpsDelete happened despite the crash');
	ok(!-e "$datadir/rmd",
		'committed FileOpsRmdir happened despite the crash');
}

# DROP TABLESPACE is safe: its XLOG_TBLSPC_DROP precedes the commit record
# and its redo removes the directories.  (In-place, so they are under
# pg_tblspc.)
$node->safe_psql('postgres',
	'SET allow_in_place_tablespaces = on; CREATE TABLESPACE ts LOCATION \'\''
);
my $tsoid = $node->safe_psql('postgres',
	"SELECT oid FROM pg_tablespace WHERE spcname = 'ts'");
ok(-d "$datadir/pg_tblspc/$tsoid", 'tablespace directory exists');
commit_and_crash('DROP TABLESPACE ts;');
is( $node->safe_psql(
		'postgres', "SELECT count(*) FROM pg_tablespace WHERE spcname = 'ts'"
	),
	'0',
	'DROP TABLESPACE committed');
ok(!-e "$datadir/pg_tblspc/$tsoid",
	'committed DROP TABLESPACE removed its directory despite the crash');

$node->stop;
done_testing();
