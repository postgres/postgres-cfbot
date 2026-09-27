# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('abort_redo');
$node->init;
$node->append_conf('postgresql.conf', "wal_level = replica\nautovacuum = off\ncheckpoint_timeout = '1h'\nmax_logical_revert_workers = 0\n");
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
});
for my $restart (0 .. 2)
{
	if ($restart)
	{
		$node->stop('immediate');
		$node->start;
	}
	ok(!-e "$dir/abort.dat", "$restart: aborted create absent");
	ok(!-e "$dir/abortdir", "$restart: aborted mkdir absent");
	is((stat("$dir/base.dat"))[2] & 07777, 0600, "$restart: original mode");
	is(-s "$dir/base.dat", 14, "$restart: original size");
	is(slurp_file("$dir/base.dat"), 'committed-base', "$restart: original bytes");
}

# Stop with a durable abort decision but no inverses applied yet.  A
# concurrently requested checkpoint must wait, not omit the abort from redo.
if ($ENV{enable_injection_points} eq 'yes')
{
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	for my $checkpoint (0, 1)
	{
		$node->safe_psql('postgres', 'CHECKPOINT');
		$node->safe_psql('postgres',
			"SELECT injection_points_attach('fileops-abort-after-flush', 'wait')");
		my $bg = $node->background_psql('postgres', on_error_stop => 0);
		$bg->query_until(qr/start/, qq(\\echo start
BEGIN;
SELECT test_fileops_create('mid.dat', 384);
SELECT test_fileops_write('mid.dat', 0, 'uncommitted');
SELECT test_fileops_truncate('base.dat', 3);
ROLLBACK;
));
		$node->wait_for_event('client backend', 'fileops-abort-after-flush');
		my $ckpt;
		if ($checkpoint)
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

