# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# A FILEOPS UNDO chain can be walked more than once: the inline rollback
# reverts part of it, fails, and hands the whole chain to the logical revert
# worker, which walks it again later.  By then the reverted files may have
# been changed again.  The second walk must leave them alone.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('fileops_atm_reapply');
$node->init;
# FILEOPS writes UNDO only when WAL is needed.  No revert worker until the
# later changes are in place.
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
});
$node->start;
$node->safe_psql('postgres',
	'CREATE EXTENSION injection_points; CREATE EXTENSION test_fileops');

my %path;
for my $f (qw(m t l))
{
	$path{$f} = $node->safe_psql('postgres',
		"SELECT test_fileops_create_tempfile('reapply_$f.dat')");
	chmod(0644, $path{$f}) or die "chmod $path{$f}: $!";
}
my $link = "$path{l}.lnk";

# One batch per operation.  The rollback applies the newest batch (the
# truncate), then the injection point fails it, and the chain goes to the ATM.
my $bg = $node->background_psql('postgres');
$bg->query_safe('SELECT injection_points_set_local()');
$bg->query_safe(
	"SELECT injection_points_attach('undo-apply-after-batch', 'error')");
$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_link('$path{l}', '$link')");
$bg->query_safe("SELECT test_fileops_chmod('$path{m}', 384)");    # 0600
$bg->query_safe("SELECT test_fileops_truncate('$path{t}', 10)");
$bg->query('ROLLBACK');
$bg->quit;

is( $node->safe_psql(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'),
	't',
	'the failed inline rollback was handed to the ATM');
is(-s $path{t}, 1024, 'truncate was reverted');
SKIP:
{
	skip 'POSIX permission semantics required', 1 if $windows_os;
	is((stat($path{m}))[2] & 07777, 0644, 'chmod was reverted');
}
ok(!-e $link, 'link was reverted');

# Later changes to the reverted files, which a second walk must not undo.
chmod(0640, $path{m}) or die "chmod $path{m}: $!";
open(my $fh, '>>', $path{t}) or die "open $path{t}: $!";
print $fh 'Y' x 1024;
close($fh);
# a different inode: a new link to the same one looks like ours
link($path{m}, $link) or die "link $link: $!";

$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok( $node->poll_query_until(
		'postgres', 'SELECT test_fileops_atm_oldest_lsn() IS NULL'),
	'revert worker drained the ATM');

is(-s $path{t}, 2048, 'later write to the truncated file survives');
SKIP:
{
	skip 'POSIX permission semantics required', 1 if $windows_os;
	is((stat($path{m}))[2] & 07777, 0640, 'later chmod survives');
}
ok(-e $link, 'later link survives');

$node->stop;
done_testing();
