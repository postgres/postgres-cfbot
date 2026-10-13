# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# Crash recovery must roll back a transaction whose UNDO was all written
# before the last checkpoint's redo pointer.  Redo starts there and so
# replays none of its UNDO batches; the checkpoint itself has to say that
# the transaction was in flight.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('fileops_ckpt_loser');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
wal_level = replica
autovacuum = off
});
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
$node->safe_psql('postgres',
	'CREATE TABLE ct (id int primary key) WITH (index_undo = on)');
$node->safe_psql('postgres', 'INSERT INTO ct SELECT generate_series(1, 10)');

my $datadir = $node->data_dir;

# Leave transactions in flight, all of their UNDO written, and checkpoint.
sub start_losers
{
	my ($tag) = @_;
	my $bg = $node->background_psql('postgres');
	$bg->query_safe('BEGIN');
	$bg->query_safe("SELECT test_fileops_create('$tag.dat', 384)");
	my $xid1 = $bg->query_safe('SELECT pg_current_xact_id()');

	my $bg2 = $node->background_psql('postgres');
	$bg2->query_safe('BEGIN');
	$bg2->query_safe('INSERT INTO ct SELECT generate_series(1001, 1300)');
	# Writes the pending index UNDO batch.
	$bg2->query_safe('SAVEPOINT s');
	my $xid2 = $bg2->query_safe('SELECT pg_current_xact_id()');

	$node->safe_psql('postgres', 'CHECKPOINT');
	$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
	return ($bg, $bg2, $xid1, $xid2);
}

my ($bg, $bg2, $xid1, $xid2) = start_losers('ckpt');
ok(-e "$datadir/ckpt.dat", 'the in-flight FILEOPS create is on disk');

my $offset = -s $node->logfile;
$node->stop('immediate');
$bg->{run}->finish;
$bg2->{run}->finish;
$node->start;

ok(!-e "$datadir/ckpt.dat",
	'crash recovery reverted a FILEOPS create made before the checkpoint');
my $log = slurp_file($node->logfile, $offset);
like(
	$log,
	qr/UNDO recovery: rolling back transaction $xid1\b/,
	'the FILEOPS transaction was found as a loser');
like(
	$log,
	qr/UNDO recovery: rolling back transaction $xid2\b/,
	'the index UNDO transaction was found as a loser');
is($node->safe_psql('postgres', 'SELECT count(*) FROM ct'),
	'10', 'the uncommitted rows are not visible');

# The same on a standby that is promoted: the standby's last restartpoint
# can likewise follow the loser's batches.
$node->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('fileops_ckpt_standby');
$standby->init_from_backup($node, 'bk', has_streaming => 1);
$standby->start;

($bg, $bg2, $xid1, $xid2) = start_losers('sb');
$node->wait_for_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');
my $sdatadir = $standby->data_dir;
ok(-e "$sdatadir/sb.dat", 'the standby replayed the in-flight create');

$offset = -s $standby->logfile;
$node->stop('immediate');
$bg->{run}->finish;
$bg2->{run}->finish;
$standby->stop('immediate');
$standby->start;
$standby->promote;

ok(!-e "$sdatadir/sb.dat",
	'promotion reverted a FILEOPS create made before the restartpoint');
$log = slurp_file($standby->logfile, $offset);
like(
	$log,
	qr/UNDO recovery: rolling back transaction $xid1\b/,
	'the standby found the FILEOPS transaction as a loser');

$standby->stop;
done_testing();
