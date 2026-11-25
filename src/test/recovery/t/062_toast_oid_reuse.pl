# Copyright (c) 2026, PostgreSQL Global Development Group

# A new TOAST value must not get the value ID of a deleted value whose chunks
# are still in the TOAST table, even when the index entries for those chunks
# are marked LP_DEAD.  A hot standby does not skip LP_DEAD entries, so it
# would find the chunks of both values.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);
$node_primary->append_conf('postgresql.conf', 'autovacuum = off');
$node_primary->start;

$node_primary->safe_psql('postgres', 'CREATE EXTENSION pageinspect');

# With STORAGE EXTERNAL, each value is stored uncompressed in two chunks.
my $toastrel = $node_primary->safe_psql(
	'postgres', q{
CREATE TABLE toast_reuse (v text)
  WITH (autovacuum_enabled = off, toast.autovacuum_enabled = off);
ALTER TABLE toast_reuse ALTER COLUMN v SET STORAGE EXTERNAL;
INSERT INTO toast_reuse VALUES (repeat('a', 3000));
SELECT reltoastrelid::regclass FROM pg_class WHERE relname = 'toast_reuse';
});
my $valueid = $node_primary->safe_psql('postgres',
	'SELECT pg_column_toast_chunk_id(v) FROM toast_reuse');

$node_primary->safe_psql('postgres', 'DELETE FROM toast_reuse');

# Flush the index page first, so that it is not being written out while the
# index scan below sets its LP_DEAD bits.
$node_primary->safe_psql('postgres', 'CHECKPOINT');

# Fetch the deleted chunks through the index.  The heap tuples are dead to
# everyone, so the scan marks their index entries LP_DEAD, while the heap
# page has too much free space to be pruned.
$node_primary->safe_psql(
	'postgres', qq{
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SELECT count(*) FROM $toastrel WHERE chunk_id = '$valueid';
});
my $ndead = $node_primary->safe_psql('postgres',
	"SELECT count(*) FROM bt_page_items('${toastrel}_index', 1) WHERE dead");
is($ndead, '2',
	'index entries of the deleted TOAST value are marked LP_DEAD');

# Make the deleted value's ID the next OID to be assigned, as if the OID
# counter had wrapped around.
$node_primary->stop;
command_ok(
	[ 'pg_resetwal', '--next-oid' => $valueid, $node_primary->data_dir ],
	'set next OID to the deleted TOAST value ID');
$node_primary->start;

$node_primary->safe_psql('postgres',
	"INSERT INTO toast_reuse VALUES (repeat('b', 3000))");
my $newid = $node_primary->safe_psql('postgres',
	'SELECT pg_column_toast_chunk_id(v) FROM toast_reuse');
isnt($newid, $valueid,
	'new TOAST value does not reuse the ID of a deleted value');

$node_primary->backup('backup');
my $node_standby = PostgreSQL::Test::Cluster->new('standby');
$node_standby->init_from_backup($node_primary, 'backup', has_streaming => 1);
$node_standby->start;
$node_primary->wait_for_replay_catchup($node_standby);

my ($ret, $stdout, $stderr) = $node_standby->psql('postgres',
	"SELECT v = repeat('b', 3000) FROM toast_reuse");
is($ret, 0, 'standby can read the new TOAST value') or diag($stderr);
is($stdout, 't', 'standby reads the new TOAST value correctly');

done_testing();
