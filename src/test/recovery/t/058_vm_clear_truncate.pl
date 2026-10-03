
# Copyright (c) 2026, PostgreSQL Global Development Group

# A standby must be able to restart when WAL it replays again clears
# visibility map bits on a VM page that a later, already replayed,
# truncation removed.  With full_page_writes off, redo cannot restore the
# VM page from an image in the clearing record, so it has to cope with the
# page not existing.  wal_consistency_checking is on so that the page redo
# recreates is checked as well.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf(
	'postgresql.conf', qq{
full_page_writes = off
wal_consistency_checking = all
autovacuum = off
});
$primary->start;
$primary->backup('bkp');

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

# Make every heap page all-visible, then make the standby create a
# restartpoint, so that a restart replays the changes below again.  The
# row lock clears the all-frozen bit of vm_upd's first page.  Otherwise
# the cross-page update below would first log a lock record that clears
# that bit, and the update would not be the first record to read the VM
# page.
$primary->safe_psql(
	'postgres', q{
CREATE TABLE vm_del (a int);
CREATE TABLE vm_hot (a int) WITH (fillfactor = 50);
CREATE TABLE vm_upd (a int);
INSERT INTO vm_del SELECT generate_series(1, 1000);
INSERT INTO vm_hot SELECT generate_series(1, 1000);
INSERT INTO vm_upd SELECT generate_series(1, 1000);
VACUUM (FREEZE) vm_del, vm_hot, vm_upd;
SELECT a FROM vm_upd WHERE a = 1 FOR UPDATE;
CHECKPOINT;
});
$primary->wait_for_replay_catchup($standby);
$standby->safe_psql('postgres', 'CHECKPOINT');

my $start_lsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

# Clear VM bits through delete, same-page update (old VM block) and
# cross-page update (new VM block), then truncate all three tables to
# zero blocks.
$primary->safe_psql(
	'postgres', q{
DELETE FROM vm_del;
UPDATE vm_hot SET a = -a WHERE a = 1;
UPDATE vm_upd SET a = -a WHERE a = 1;
DELETE FROM vm_hot;
DELETE FROM vm_upd;
VACUUM vm_del, vm_hot, vm_upd;
});
is( $primary->safe_psql(
		'postgres',
		"SELECT sum(pg_relation_size(c, 'vm')) FROM unnest('{vm_del,vm_hot,vm_upd}'::regclass[]) c"
	),
	'0',
	'VMs truncated on primary');
$primary->wait_for_replay_catchup($standby);

$standby->stop;
my $log_offset = -s $standby->logfile;
my $ret = $standby->start(fail_ok => 1);

my $log = slurp_file($standby->logfile, $log_offset);
my ($redo_lsn) = $log =~ /redo starts at ([0-9A-F]+\/[0-9A-F]+)/;
ok( defined($redo_lsn)
	  && $primary->safe_psql('postgres',
		"SELECT '$redo_lsn'::pg_lsn < '$start_lsn'::pg_lsn") eq 't',
	'redo after restart starts before the VM bits were cleared');
ok($ret, 'standby restarts after replaying VM truncation');
unlike($log, qr/invalid pages/, 'no invalid page references in standby log');

done_testing();
