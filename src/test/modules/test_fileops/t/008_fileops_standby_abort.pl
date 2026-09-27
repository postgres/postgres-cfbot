# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "max_prepared_transactions = 5\n");
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
$primary->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'backup', has_streaming => 1);
$standby->start;
$primary->safe_psql('postgres', q{
BEGIN;
SELECT test_fileops_create('base.dat', 384);
SELECT test_fileops_write('base.dat', 0, 'committed-base');
COMMIT;
BEGIN;
SELECT test_fileops_create('abort.dat', 384);
SELECT test_fileops_write('abort.dat', 0, 'aborted');
ROLLBACK;
BEGIN; SELECT test_fileops_mkdir('abortdir', 448); ROLLBACK;
BEGIN; SELECT test_fileops_chmod('base.dat', 448); ROLLBACK;
BEGIN; SELECT test_fileops_write('base.dat', 0, 'OVERWRITTEN!!!'); ROLLBACK;
BEGIN; SELECT test_fileops_truncate('base.dat', 3); ROLLBACK;
});
$primary->safe_psql('postgres', q{
BEGIN;
SELECT test_fileops_write('base.dat', 0, 'prepared');
SELECT test_fileops_truncate('base.dat', 2);
PREPARE TRANSACTION 'p';
ROLLBACK PREPARED 'p';
BEGIN;
SAVEPOINT a;
SELECT test_fileops_chmod('base.dat', 448);
ROLLBACK TO a;
COMMIT;
});
$primary->wait_for_catchup($standby);
for my $stage ('streaming', 'promoted')
{
	$standby->promote if $stage eq 'promoted';
	for my $node ($primary, $standby)
	{
		my $dir = $node->data_dir;
		my $name = $node->name;
		ok(!-e "$dir/abort.dat", "$stage $name: aborted create absent");
		ok(!-e "$dir/abortdir", "$stage $name: aborted mkdir absent");
		is((stat("$dir/base.dat"))[2] & 07777, 0600, "$stage $name: original mode");
		is(slurp_file("$dir/base.dat"), 'committed-base', "$stage $name: original bytes");
	}
}
$standby->stop;
$primary->stop;
done_testing();
