# Copyright (c) 2026, PostgreSQL Global Development Group

# A transaction that the crash leaves in progress must be rolled back even
# when all of its UNDO precedes the last checkpoint's redo pointer.  Redo
# starts there, so it sees the chain only through the checkpoint's
# XLOG_UNDO_INFLIGHT record.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('checkpoint_loser');
$node->init;
$node->append_conf('postgresql.conf',
	"wal_level = replica\nautovacuum = off\ncheckpoint_timeout = '1h'\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
my $pd = $node->data_dir;
foreach my $ckpt (0, 1)
{
	my $f = "loser_$ckpt.dat";
	my $bg = $node->background_psql('postgres');
	$bg->query_until(qr/ready/,
		"BEGIN;\nSELECT test_fileops_create('$f', 384);\n\\echo ready\n");
	$node->safe_psql('postgres', 'CHECKPOINT') if $ckpt;

	# Crash before closing the client, so the backend cannot roll back first.
	$node->stop('immediate');
	$bg->{run}->kill_kill;
	$node->start;
	ok(!-e "$pd/$f",
		"checkpoint=$ckpt: the loser's file is removed after a crash");
}

$node->stop;
done_testing();
