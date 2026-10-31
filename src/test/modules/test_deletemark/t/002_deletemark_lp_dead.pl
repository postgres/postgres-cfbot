# Copyright (c) 2022-2026, PostgreSQL Global Development Group

# nbtree delete-marking and the LP_DEAD hint.
#
# (1) Delete-marking an entry that already carries LP_DEAD must not produce an
#     LP_DEAD tombstone: _bt_simpledel_pass asserts that a tombstone is never
#     LP_DEAD, and an LP_DEAD tombstone would also be skipped by the very
#     old-snapshot scans it exists for.
# (2) Demoting a tombstone back to a plain entry (rollback) must leave a live
#     entry on the primary and on a streaming standby alike: the primary must
#     clear LP_DEAD just as redo does, or the restored entry stays hidden from
#     index scans on the primary only.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', 'wal_consistency_checking = btree');
$primary->start;
$primary->safe_psql('postgres', 'CREATE EXTENSION test_deletemark');
$primary->safe_psql('postgres', 'CREATE EXTENSION amcheck');
$primary->safe_psql(
	'postgres', q{
CREATE TABLE dm_d (k int, v int) WITH (autovacuum_enabled = off);
INSERT INTO dm_d SELECT g, g FROM generate_series(1, 20) g;
CREATE INDEX dm_didx ON dm_d (k);
});

$primary->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bk', has_streaming => 1);
$standby->start;

# (2) An LP_DEAD tombstone demoted by a rollback.
my $tid15 =
  $primary->safe_psql('postgres', "SELECT ctid FROM dm_d WHERE k = 15");
is($primary->safe_psql('postgres', "SELECT dm_mark('dm_didx', '$tid15')"),
	't', 'entry for k=15 delete-marked');
is( $primary->safe_psql(
		'postgres', "SELECT dm_set_dead('dm_didx', '$tid15')"),
	't',
	'LP_DEAD set on tombstone');
is( $primary->safe_psql('postgres', "SELECT dm_unmark('dm_didx', '$tid15')"),
	't',
	'tombstone demoted');
is( $primary->safe_psql('postgres', "SELECT dm_is_dead('dm_didx', '$tid15')"),
	'f',
	'demoted entry is not LP_DEAD on the primary');

$primary->wait_for_catchup($standby, 'replay');
is( $standby->safe_psql('postgres', "SELECT dm_is_dead('dm_didx', '$tid15')"),
	'f',
	'demoted entry is not LP_DEAD on the standby');

my $q = "SET enable_seqscan = off; SET enable_bitmapscan = off; "
  . "SELECT count(*) FROM dm_d WHERE k = 15";
my $p = $primary->safe_psql('postgres', $q);
my $s = $standby->safe_psql('postgres', $q);
is($p, '1', 'primary index scan finds the restored entry');
is($s, $p, 'standby index scan agrees with the primary');


# (1) LP_DEAD plain entry, then delete-mark it.
my $tid10 =
  $primary->safe_psql('postgres', "SELECT ctid FROM dm_d WHERE k = 10");
is( $primary->safe_psql(
		'postgres', "SELECT dm_set_dead('dm_didx', '$tid10')"),
	't',
	'LP_DEAD set on plain entry');
is($primary->safe_psql('postgres', "SELECT dm_mark('dm_didx', '$tid10')"),
	'f', 'delete-marking an LP_DEAD entry reports "not present"');
is( $primary->safe_psql(
		'postgres', "SELECT dm_classify('dm_didx', '$tid10')"),
	'plain',
	'LP_DEAD entry was not turned into a tombstone');

# Fill the leaf so an insert runs the LP_DEAD simple-deletion pass over it.
my ($ret, $out, $err) = $primary->psql('postgres',
	"INSERT INTO dm_d SELECT g, g FROM generate_series(21, 1500) g");
is($ret, 0, 'insert that runs _bt_simpledel_pass succeeds');
is($primary->safe_psql('postgres', "SELECT bt_index_check('dm_didx')"),
	'', 'bt_index_check clean after simple deletion');

$standby->stop;
$primary->stop;
done_testing();
