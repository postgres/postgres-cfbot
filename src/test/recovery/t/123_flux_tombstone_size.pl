# Copyright (c) 2026, PostgreSQL Global Development Group
#
# The largest entry a delete-marking table's nbtree index accepts must still
# fit after it becomes a tombstone.  Delete-marking grows a leaf entry by a
# MAXALIGN'd heap TID trailer, so an entry right at BTMaxItemSize would turn
# into a leaf tuple larger than the limit (which amcheck reports), or the
# UPDATE that delete-marks it would fail.  Such a key has to be rejected when
# it is inserted, like any other key that is too long.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('flux_tombstone_size');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION amcheck');

# BTMaxItemSize, assuming MAXALIGN 8: one third of the usable page space,
# rounded down to MAXALIGN, less one MAXALIGN'd heap TID.  A text key of L
# bytes (4-byte varlena header, not compressed) makes a leaf entry of
# MAXALIGN(8 + 4 + L) bytes.
my $bs = $node->safe_psql('postgres', "SELECT current_setting('block_size')");
my $max = int(($bs - 40 - 16) / 3 / 8) * 8 - 8;
my $big = $max - 12;    # entry of exactly BTMaxItemSize
my $ok = $big - 8;      # entry of BTMaxItemSize - 8: its tombstone fits

# Random hex does not compress, so the index entry keeps its full size.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION rnd(n int) RETURNS text LANGUAGE sql AS
$$ SELECT left(string_agg(md5(random()::text), ''), n)
   FROM generate_series(1, n / 32 + 1) $$;
CREATE TABLE t (id int, k text) USING flux;
CREATE INDEX t_k ON t (k);
});

my ($rc, $stdout, $stderr) =
  $node->psql('postgres', "INSERT INTO t VALUES (1, rnd($big))");
like(
	$stderr,
	qr/index row size \d+ exceeds btree version 4 maximum/,
	'a key whose tombstone would not fit is rejected at insert');

$node->safe_psql('postgres', "INSERT INTO t VALUES (2, rnd($ok))");
my $ctid = $node->safe_psql('postgres', 'SELECT ctid FROM t WHERE id = 2');

# Change every row's key in place; each old entry becomes a tombstone.
$node->safe_psql('postgres', "UPDATE t SET k = rnd(length(k))");
is($node->safe_psql('postgres', 'SELECT ctid FROM t WHERE id = 2'),
	$ctid, 'the key UPDATE kept the TID');

($rc, $stdout, $stderr) =
  $node->psql('postgres', "SELECT bt_index_check('t_k', true)");
is($rc . $stderr, '0', 'bt_index_check passes after the key UPDATE');

# CREATE INDEX enforces the same limit.
$node->safe_psql(
	'postgres', qq{
CREATE TABLE b (id int, k text) USING flux;
INSERT INTO b VALUES (1, rnd($big));
});
($rc, $stdout, $stderr) =
  $node->psql('postgres', 'CREATE INDEX b_k ON b (k)');
like(
	$stderr,
	qr/index row size \d+ exceeds btree version 4 maximum/,
	'CREATE INDEX rejects a key whose tombstone would not fit');

# A heap table keeps the full limit.
$node->safe_psql(
	'postgres', qq{
CREATE TABLE h (id int, k text) USING heap;
CREATE INDEX h_k ON h (k);
INSERT INTO h VALUES (1, rnd($big));
});
pass('a heap table accepts a key of exactly BTMaxItemSize');

$node->stop;
done_testing();
