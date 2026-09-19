# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Injection points not supported by this build'
  unless $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('ownership');
$node->init;
$node->append_conf('postgresql.conf', q{
wal_level = replica
autovacuum = off
max_logical_revert_workers = 0
logical_revert_naptime = 100ms
});
$node->start;
$node->safe_psql('postgres', q{
CREATE EXTENSION injection_points;
CREATE EXTENSION test_fileops;
CREATE EXTENSION test_idxundo;
CREATE TABLE t (id int primary key) WITH (index_undo = on);
CHECKPOINT;
});

# This point must precede the abort decision.  Otherwise recovery forgets
# the non-FILEOPS chain before it was applied or registered in the ATM.
$node->safe_psql('postgres',
 "SELECT injection_points_attach('undo-xact-abort-before-apply', 'wait')");
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_until(qr/start/, qq(\\echo start
BEGIN;
INSERT INTO t SELECT generate_series(1, 10);
SELECT test_fileops_create('first.dat', 384);
ROLLBACK;
));
$node->wait_for_event('client backend', 'undo-xact-abort-before-apply');
$node->stop('immediate');
$bg->{run}->finish;
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->start;
ok($node->poll_query_until('postgres',
 'SELECT test_fileops_atm_oldest_lsn() IS NULL'), 'crash rollback drained');
# The worker walks the index chain but leaves its entries for VACUUM (see
# nbtree_undo_apply()).
is($node->safe_psql('postgres',
 "SELECT idxundo_count_dead_all('t_pkey')"), '0',
 'crash before generic apply leaves the index entries for VACUUM');
ok(!-e $node->data_dir . '/first.dat', 'crash rollback removes file');

$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 0');
$node->restart;
$bg = $node->background_psql('postgres');
$bg->query_safe('SELECT injection_points_set_local()');
$bg->query_safe("SELECT injection_points_attach('undo-apply-after-batch', 'error')");
$bg->query_safe('BEGIN; SAVEPOINT s');
$bg->query_safe("SELECT test_fileops_create('reuse.dat', 384)");
$bg->query_safe('INSERT INTO t VALUES (11)');
$bg->query('ROLLBACK TO s');
$bg->query_safe('COMMIT');
$bg->quit;
is($node->safe_psql('postgres',
 'SELECT test_fileops_atm_oldest_lsn() IS NOT NULL'), 't',
 'failed bounded walk is queued after parent commits');
ok(!-e $node->data_dir . '/reuse.dat', 'subabort removed file');
$node->safe_psql('postgres', q{
BEGIN;
SELECT test_fileops_create('reuse.dat', 384);
SELECT test_fileops_write('reuse.dat', 0, 'T2-committed');
COMMIT;
CHECKPOINT;
});
# The checkpoint/restart also checks that ownership survives ATM persistence.
$node->append_conf('postgresql.conf', 'max_logical_revert_workers = 2');
$node->restart;
ok($node->poll_query_until('postgres',
 'SELECT test_fileops_atm_oldest_lsn() IS NULL'), 'bounded ATM entry drained');
my $path = $node->data_dir . '/reuse.dat';
is(-e $path ? slurp_file($path) : '<missing>', 'T2-committed',
 'bounded worker leaves committed same-path reuse intact');
# The inline walk killed entry 11 before the injected error; the worker
# leaves index entries alone.
is($node->safe_psql('postgres',
 "SELECT idxundo_count_dead_all('t_pkey')"), '1',
 'only the inline subtransaction walk marked an index entry dead');
$node->safe_psql('postgres',
 "SELECT injection_points_attach('fileops-write-short', 'error')");
my ($ret, $out, $err) = $node->psql('postgres',
 "SELECT test_fileops_write('reuse.dat', 0, 'partialwrite')");
like($err, qr/could not write/, 'short write reports error');
$node->safe_psql('postgres',
 "SELECT injection_points_detach('fileops-write-short')");
is(slurp_file($path), 'T2-committed', 'short write restores before-image');
# A first write inside a savepoint must assign the subxid before failing.
$bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN; SAVEPOINT short_write');
$node->safe_psql('postgres',
 "SELECT injection_points_attach('fileops-write-short', 'error')");
$bg->query("SELECT test_fileops_write('reuse.dat', 20, 'extension')");
like($bg->{stderr}, qr/could not write/, 'short extending write reports error');
$bg->{stderr} = '';
$bg->query_safe('ROLLBACK TO short_write; COMMIT');
$bg->quit;
$node->safe_psql('postgres',
 "SELECT injection_points_detach('fileops-write-short')");
is(slurp_file($path), 'T2-committed', 'short extending subxact write restores length');
$node->stop;
done_testing();
