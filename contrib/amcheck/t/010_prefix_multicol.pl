# Copyright (c) 2026, PostgreSQL Global Development Group

# Prefix compression of a key with more than one variable-length column.  When
# a run of rows shares a leading text column whole and shares a long prefix of
# the next text column, the coder shares the leading column across the next
# column's length-header boundary (lead_shared > 0) and still carves only the
# divergent column's header.  The earlier format shared nothing past the first
# column's differing header, so lead_shared was always zero.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

$node->safe_psql('postgres',
	'CREATE EXTENSION amcheck; CREATE EXTENSION pageinspect;');

# Rows share the whole first column (a) and a long, length-varying prefix of
# the second (b).  b varies only in its trailing counter, so every b differs in
# length behind a 90-byte common prefix: the earlier format stops sharing at
# b's length header, v17 shares a fully and b's prefix.
$node->safe_psql(
	'postgres', q{
CREATE TABLE mc (a text COLLATE "C", b text COLLATE "C");
INSERT INTO mc
  SELECT '/srv/objects/customer-0042/archive',
         '/path/segment/' || repeat('x', 76) || '/' || g
  FROM generate_series(1, 4000) g;
CREATE INDEX mc_on  ON mc (a, b) WITH (prefix_compression = on);
CREATE INDEX mc_off ON mc (a, b) WITH (prefix_compression = off);
});

# Every data leaf of mc_on is coded (BTP_PREFIX is 512).
is( $node->safe_psql(
		'postgres', q{
SELECT count(*) FILTER (WHERE (btpo_flags & 512) = 0) = 0
       AND count(*) FILTER (WHERE (btpo_flags & 512) <> 0) > 0
  FROM generate_series(1, pg_relation_size('mc_on') / 8192 - 1) b,
       LATERAL bt_page_stats('mc_on', b) s
 WHERE s.type = 'l';
}),
	't',
	'every data leaf of the multicolumn index is coded');

# Compression pays off: the coded index is materially smaller than the plain
# one.  The saving comes from sharing a and b's common prefix together.
is( $node->safe_psql(
		'postgres', q{
SELECT pg_relation_size('mc_on') * 4 < pg_relation_size('mc_off') * 3;
}),
	't',
	'multicolumn prefix compression shrinks the index');

# amcheck with heapallindexed compares every indexed datum with the heap, so a
# decode that lost or altered a byte would fail here.
is( $node->safe_psql('postgres',
		"SELECT bt_index_parent_check('mc_on', true, true)"),
	'',
	'multicolumn coded index passes bt_index_parent_check(heapallindexed)');

# An index-only scan returns bytes straight from the coded leaves; a heap
# sequential scan returns the stored heap bytes.  They must agree exactly,
# including b's length, which the counter varies.
my $digest = q{md5(string_agg(a || '\x00' || b, '|' ORDER BY a, b))};
my $ios = $node->safe_psql('postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = on; SET enable_indexscan = off;
SELECT count(*), $digest FROM mc;
});
my $heap = $node->safe_psql('postgres', qq{
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*), $digest FROM mc;
});
is($ios, $heap, 'index-only scan is byte-identical to the heap');

like(
	$node->safe_psql('postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = on; SET enable_indexscan = off;
EXPLAIN (COSTS OFF) SELECT count(*), $digest FROM mc;
}),
	qr/Index Only Scan using mc_on/,
	'digest query uses an index-only scan on the coded index');

# The format change itself, read from the live page through pageinspect: every
# coded member carries lead_shared > 0, meaning the coder shared the first
# column across the second column's length header.  The earlier single-column
# format always stored lead_shared = 0, so this subtest fails if the
# multi-column walk stops engaging.
#
# A coded member's item is [IndexTupleData 8 bytes][BTPrefixCode], and
# BTPrefixCode is group@+0, lead_shared@+2, shared_len@+4, so lead_shared sits
# at item byte +10 as a native-endian uint16.  get_raw_page gives the raw page;
# the line pointer at page offset 24 + 4*(offnum-1) packs lp_off (bits 0..14)
# and lp_len (bits 17..31).  bt_page_items reports the same length as itemlen,
# so lp_len = itemlen cross-checks the hand-parsed line pointer before the +10
# read is trusted.  Items 1 (high key) and 2 (prefix directory) are skipped;
# coded members start at offset 3 and are at least 16 bytes.  The anchor's
# reference member shares b whole (lead_shared = total data-area bytes); the
# rest share a (35 bytes) plus b's common value prefix, so lead_shared is 35.
my $leadcheck = $node->safe_psql(
	'postgres', q{
WITH leaves AS (
  SELECT blk
    FROM generate_series(1, pg_relation_size('mc_on') / 8192 - 1) blk
   WHERE (bt_page_stats('mc_on', blk::int)).type = 'l'
     AND ((bt_page_stats('mc_on', blk::int)).btpo_flags & 512) <> 0
),
raw AS (SELECT blk, get_raw_page('mc_on', blk::int) AS pg FROM leaves),
lp AS (
  SELECT r.blk, r.pg, off,
         (v & 32767) AS lp_off,
         (v >> 17)   AS lp_len
    FROM raw r,
         LATERAL (SELECT get_byte(r.pg, 12) + get_byte(r.pg, 13) * 256
                    AS pd_lower) hdr,
         LATERAL generate_series(3, (hdr.pd_lower - 24) / 4) off,
         LATERAL (SELECT get_byte(r.pg, 24 + 4 * (off - 1))
                       + get_byte(r.pg, 24 + 4 * (off - 1) + 1) * 256
                       + get_byte(r.pg, 24 + 4 * (off - 1) + 2) * 65536
                       + get_byte(r.pg, 24 + 4 * (off - 1) + 3) * 16777216
                    AS v) t
),
member AS (
  SELECT blk, off, lp_off, lp_len,
         get_byte(pg, lp_off + 10) + get_byte(pg, lp_off + 11) * 256
           AS lead_shared
    FROM lp
   WHERE lp_len >= 16
),
xcheck AS (
  SELECT bool_and(m.lp_len = i.itemlen) AS ok
    FROM member m
         JOIN LATERAL bt_page_items('mc_on', m.blk::int) i
           ON i.itemoffset = m.off
)
SELECT (SELECT count(*) FROM member) > 0
   AND (SELECT count(*) FILTER (WHERE lead_shared = 0) FROM member) = 0
   AND (SELECT min(lead_shared) FROM member) = 35
   AND (SELECT ok FROM xcheck);
});
is($leadcheck, 't',
	'every coded member shares a leading column past the next column header');

# Straddle the varlena short/long header boundary.  b keeps a 118-byte common
# prefix, then a run of 'q' whose length (2..18) carries the total across 127,
# then '/' and the unique counter.  Members below 127 bytes store a one-byte
# length header, members at or above store a four-byte one, so one group mixes
# both header widths.  Decode splices the shared value after each member's own
# header using the anchor's header width; a one-byte error there (ahdr off by
# one) reads the anchor's shared bytes shifted by a byte and corrupts a byte
# that survived into the reconstructed key.  amcheck heapallindexed and the
# index-only-scan digest below both catch that corruption.
$node->safe_psql(
	'postgres', q{
CREATE TABLE sm (a text COLLATE "C", b text COLLATE "C");
INSERT INTO sm
  SELECT 'const-a',
         repeat('p', 118) || repeat('q', (g % 17) + 2) || '/' || g
  FROM generate_series(1, 4000) g;
CREATE INDEX sm_on ON sm (a, b) WITH (prefix_compression = on);
});

# Both header widths occur, and the index is coded.
is( $node->safe_psql(
		'postgres', q{
SELECT count(*) FILTER (WHERE length(b) <= 126) > 0
       AND count(*) FILTER (WHERE length(b) >= 127) > 0
  FROM sm;
}),
	't',
	'straddle data mixes one-byte and four-byte varlena headers');

is( $node->safe_psql(
		'postgres', q{
SELECT count(*) FILTER (WHERE (btpo_flags & 512) = 0) = 0
       AND count(*) FILTER (WHERE (btpo_flags & 512) <> 0) > 0
  FROM generate_series(1, pg_relation_size('sm_on') / 8192 - 1) b,
       LATERAL bt_page_stats('sm_on', b) s
 WHERE s.type = 'l';
}),
	't',
	'every data leaf of the straddle index is coded');

is( $node->safe_psql('postgres',
		"SELECT bt_index_parent_check('sm_on', true, true)"),
	'',
	'straddle coded index passes bt_index_parent_check(heapallindexed)');

my $sm_digest = q{md5(string_agg(a || '\x00' || b, '|' ORDER BY a, b))};
my $sm_ios = $node->safe_psql('postgres', qq{
SET enable_seqscan = off; SET enable_bitmapscan = off;
SET enable_indexonlyscan = on; SET enable_indexscan = off;
SELECT count(*), $sm_digest FROM sm;
});
my $sm_heap = $node->safe_psql('postgres', qq{
SET enable_indexscan = off; SET enable_indexonlyscan = off;
SET enable_bitmapscan = off; SET enable_seqscan = on;
SELECT count(*), $sm_digest FROM sm;
});
is($sm_ios, $sm_heap,
	'straddle index-only scan is byte-identical to the heap');

$node->stop;
done_testing();
