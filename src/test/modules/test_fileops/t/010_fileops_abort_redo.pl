# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('abort_redo');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = replica\nautovacuum = off\ncheckpoint_timeout = '1h'\nmax_logical_revert_workers = 0\nmax_prepared_transactions = 5\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
my $dir = $node->data_dir;
$node->safe_psql('postgres', q{
BEGIN;
SELECT test_fileops_create('base.dat', 384);
SELECT test_fileops_write('base.dat', 0, 'committed-base');
COMMIT;
CHECKPOINT;
BEGIN;
SELECT test_fileops_create('abort.dat', 384);
SELECT test_fileops_write('abort.dat', 0, 'aborted');
ROLLBACK;
BEGIN; SELECT test_fileops_mkdir('abortdir', 448); ROLLBACK;
BEGIN; SELECT test_fileops_chmod('base.dat', 448); ROLLBACK;
BEGIN; SELECT test_fileops_write('base.dat', 0, 'OVERWRITTEN!!!'); ROLLBACK;
BEGIN; SELECT test_fileops_truncate('base.dat', 3); ROLLBACK;
BEGIN;
SAVEPOINT s;
SELECT test_fileops_create('sub.dat', 384);
ROLLBACK TO s;
SELECT test_fileops_write('base.dat', 0, 'parent');
ROLLBACK;
BEGIN; SELECT test_fileops_symlink('base.dat', 'abort.sym'); ROLLBACK;
BEGIN; SELECT test_fileops_link('base.dat', 'abort.link'); ROLLBACK;

});
my $xattrs = $node->safe_psql('postgres',
 "SELECT test_fileops_setxattr('base.dat', 'user.abort_test', 'original')") eq 't';
if ($xattrs)
{
	$node->safe_psql('postgres', q{
BEGIN;
SELECT test_fileops_setxattr('base.dat', 'user.abort_test', 'changed');
ROLLBACK;
BEGIN;
SELECT test_fileops_removexattr('base.dat', 'user.abort_test');
ROLLBACK;
});
}
my ($uid, $gid) = (stat("$dir/base.dat"))[4, 5];
# An unprivileged test can chown to its own identity; a different uid needs
# a privileged harness.  This still exercises CHOWN inverse serialization.
$node->safe_psql('postgres',
 "BEGIN; SELECT test_fileops_chown('base.dat', $uid, $gid); ROLLBACK;");
for my $restart (0 .. 2)
{
	if ($restart)
	{
		$node->stop('immediate');
		$node->start;
	}
	ok(!-e "$dir/sub.dat", "$restart: subabort then parent abort");
	ok(!-l "$dir/abort.sym", "$restart: symlink inverse");
	ok(!-e "$dir/abort.link", "$restart: link inverse");
	ok(!-e "$dir/abort.dat", "$restart: aborted create absent");
	ok(!-e "$dir/abortdir", "$restart: aborted mkdir absent");
	is((stat("$dir/base.dat"))[2] & 07777, 0600, "$restart: original mode");
	is((stat("$dir/base.dat"))[4], $uid, "$restart: owner after chown inverse");
	is((stat("$dir/base.dat"))[5], $gid, "$restart: group after chown inverse");
	SKIP:
	{
		skip 'extended attributes not supported', 1 unless $xattrs;
		is($node->safe_psql('postgres',
		 "SELECT test_fileops_getxattr('base.dat', 'user.abort_test')"),
		 'original', "$restart: xattr inverses restore value");
	}
	is(-s "$dir/base.dat", 14, "$restart: original size");
	is(slurp_file("$dir/base.dat"), 'committed-base', "$restart: original bytes");
}

# Stop with a durable abort decision but no inverses applied yet.  A
# concurrently requested checkpoint must wait, not omit the abort from redo.
if ($ENV{enable_injection_points} eq 'yes')
{
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	for my $checkpoint (0, 1, 2)
	{
		my $finish = $checkpoint == 2 ? "PREPARE TRANSACTION 'window'; ROLLBACK PREPARED 'window';" : 'ROLLBACK;';
		$node->safe_psql('postgres', 'CHECKPOINT');
		$node->safe_psql('postgres',
			"SELECT injection_points_attach('fileops-abort-after-flush', 'wait')");
		my $bg = $node->background_psql('postgres', on_error_stop => 0);
		$bg->query_until(qr/start/, qq(\\echo start
BEGIN;
SELECT test_fileops_create('mid.dat', 384);
SELECT test_fileops_write('mid.dat', 0, 'uncommitted');
SELECT test_fileops_truncate('base.dat', 3);
$finish
));
		$node->wait_for_event('client backend', 'fileops-abort-after-flush');
		my $ckpt;
		if ($checkpoint == 1)
		{
			$ckpt = $node->background_psql('postgres', on_error_stop => 0);
			$ckpt->query_until(qr/checkpoint/, "\\echo checkpoint\nCHECKPOINT;\n");
			$node->wait_for_event('checkpointer', 'CheckpointDelayStart');
		}
		$node->stop('immediate');
		$bg->{run}->finish;
		$ckpt->{run}->finish if $ckpt;
		$node->start;
		ok(!-e "$dir/mid.dat", "$checkpoint: interrupted abort removed create");
		is(slurp_file("$dir/base.dat"), 'committed-base',
			"$checkpoint: interrupted abort restored shrink bytes");
		$node->stop('immediate');
		$node->start;
		ok(!-e "$dir/mid.dat", "$checkpoint: repeated recovery keeps create absent");
	}
}

if ($ENV{enable_injection_points} eq 'yes')
{
	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', q{
SELECT injection_points_attach('createdb-wal-log-after-copy-relation', 'error');
SELECT injection_points_attach('fileops-abort-after-flush', 'wait');
});
	my $db = $node->background_psql('postgres', on_error_stop => 0);
	$db->query_until(qr/start/, "\\echo start\nCREATE DATABASE aborted_tree STRATEGY WAL_LOG OID 90000;\n");
	$node->wait_for_event('client backend', 'fileops-abort-after-flush');
	# createdb's error cleanup removes the online directory already.  Redo
	# recreates it from forward WAL, so the embedded tree inverse must remove
	# it again even though the original inode no longer exists.
	ok(!-e "$dir/base/90000", 'createdb error cleanup removed online tree');
	$node->stop('immediate');
	$db->{run}->finish;
	$node->start;
	ok(!-e "$dir/base/90000", 'abort redo removes database tree');
}

# A WARNING is not a durable retry obligation: fixing permissions does not
# retry this inverse online.  A crash before the next checkpoint replays it.
$node->safe_psql('postgres', 'CHECKPOINT');
my $failed = $node->background_psql('postgres');
$failed->query_safe('BEGIN');
$failed->query_safe("SELECT test_fileops_truncate('base.dat', 3)");
chmod(0400, "$dir/base.dat") or die "chmod: $!";
my $log_offset = -s $node->logfile;
$failed->query('ROLLBACK');
$failed->quit;
like(slurp_file($node->logfile, $log_offset),
	qr/WARNING:.*FILEOPS UNDO TRUNCATE: could not open/,
	'failed inverse reports a warning');
chmod(0600, "$dir/base.dat") or die "chmod: $!";
is(slurp_file("$dir/base.dat"), 'com', 'failed inverse remains unapplied online');
$node->stop('immediate');
$node->start;
is(slurp_file("$dir/base.dat"), 'committed-base',
	'abort redo retries failed inverse after crash');

# Refuse excessive before-images before touching the file.
my ($ret, $out, $err) = $node->psql('postgres',
	"SELECT test_fileops_write('base.dat', 0, repeat('x', 1048577))");
like($err, qr/ERROR:.*excessive transactional file write range/,
	'oversized write is an ordinary error');
is(slurp_file("$dir/base.dat"), 'committed-base', 'refused write changes nothing');
$node->stop;
done_testing();
