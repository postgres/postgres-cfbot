# Copyright (c) 2026, PostgreSQL Global Development Group

# Live logical replication of HOT-indexed updates under non-default replica
# identities.  The apply worker locates the row to update or delete via the
# replica identity: a seqscan for REPLICA IDENTITY FULL, and the nominated
# index for REPLICA IDENTITY USING INDEX.  On a subscriber whose tables carry
# extra indexes (so apply performs HOT-indexed updates and leaves stale index
# leaves), that lookup must still find the current row, including after the
# identity column's value is cycled away and back (ABA).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $publisher = PostgreSQL::Test::Cluster->new('publisher');
$publisher->init(allows_streaming => 'logical');
$publisher->start;

my $subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$subscriber->init;
$subscriber->start;

my $pub_conninfo = $publisher->connstr . ' dbname=postgres';

# tab_full: REPLICA IDENTITY FULL (apply uses a sequential scan).
# tab_idx:  REPLICA IDENTITY USING INDEX on a non-PK unique index whose
#           column is itself updated, so the apply-side index lookup must
#           tolerate stale leaves left by earlier HOT-indexed updates.
$publisher->safe_psql('postgres', q{
	CREATE TABLE tab_full (a int, b int, c int);
	ALTER TABLE tab_full REPLICA IDENTITY FULL;
	CREATE TABLE tab_idx (k int NOT NULL, v int, w int);
	CREATE UNIQUE INDEX tab_idx_k ON tab_idx (k);
	ALTER TABLE tab_idx REPLICA IDENTITY USING INDEX tab_idx_k;
	CREATE PUBLICATION pub FOR TABLE tab_full, tab_idx;
});

# The subscriber adds extra secondary indexes so that an UPDATE changing one
# indexed column stays HOT-indexed on apply.
$subscriber->safe_psql('postgres', q{
	CREATE TABLE tab_full (a int, b int, c int) WITH (fillfactor = 50);
	CREATE INDEX tab_full_b ON tab_full (b);
	CREATE INDEX tab_full_c ON tab_full (c);
	ALTER TABLE tab_full REPLICA IDENTITY FULL;
	CREATE TABLE tab_idx (k int NOT NULL, v int, w int) WITH (fillfactor = 50);
	CREATE UNIQUE INDEX tab_idx_k ON tab_idx (k);
	CREATE INDEX tab_idx_v ON tab_idx (v);
	CREATE INDEX tab_idx_w ON tab_idx (w);
	ALTER TABLE tab_idx REPLICA IDENTITY USING INDEX tab_idx_k;
});

# Allow HOT-indexed updates on the apply path.
$subscriber->safe_psql('postgres', qq{
	CREATE SUBSCRIPTION sub
	  CONNECTION '$pub_conninfo'
	  PUBLICATION pub
	  WITH (hot_indexed_on_apply = always);
});
$subscriber->wait_for_subscription_sync($publisher, 'sub');

# Seed both tables.
$publisher->safe_psql('postgres', q{
	INSERT INTO tab_full VALUES (1, 10, 100), (2, 20, 200);
	INSERT INTO tab_idx VALUES (1, 10, 1000), (2, 20, 2000);
});
$publisher->wait_for_catchup('sub');

# A run of single-column updates: each stays HOT-indexed on the subscriber and
# leaves stale leaves, then the identity/PK row is matched again by the next
# change.  Include an ABA cycle on the USING INDEX column k (1 -> 3 -> 1).
$publisher->safe_psql('postgres', q{
	UPDATE tab_full SET b = b + 1 WHERE a = 1;
	UPDATE tab_full SET c = c + 1 WHERE a = 1;
	UPDATE tab_full SET b = b + 1 WHERE a = 1;
	UPDATE tab_idx SET v = v + 1 WHERE k = 1;
	UPDATE tab_idx SET k = 3 WHERE k = 1;
	UPDATE tab_idx SET w = w + 1 WHERE k = 3;
	UPDATE tab_idx SET k = 1 WHERE k = 3;
	DELETE FROM tab_full WHERE a = 2;
	DELETE FROM tab_idx WHERE k = 2;
});
$publisher->wait_for_catchup('sub');

# The subscriber must match the publisher exactly: the RI lookups found the
# right rows across the HOT-indexed chains and the ABA cycle.
my $pub_full = $publisher->safe_psql('postgres',
	q{SELECT a, b, c FROM tab_full ORDER BY a});
my $sub_full = $subscriber->safe_psql('postgres',
	q{SELECT a, b, c FROM tab_full ORDER BY a});
is($sub_full, $pub_full, 'REPLICA IDENTITY FULL: subscriber matches publisher');

my $pub_idx = $publisher->safe_psql('postgres',
	q{SELECT k, v, w FROM tab_idx ORDER BY k});
my $sub_idx = $subscriber->safe_psql('postgres',
	q{SELECT k, v, w FROM tab_idx ORDER BY k});
is($sub_idx, $pub_idx,
	'REPLICA IDENTITY USING INDEX: subscriber matches publisher across ABA');

# The subscriber's tables must be structurally consistent (stubs recognised).
$subscriber->safe_psql('postgres', q{CREATE EXTENSION amcheck});
is( $subscriber->safe_psql('postgres',
		q{SELECT count(*) FROM verify_heapam('tab_idx')}),
	'0',
	'subscriber tab_idx has no heap corruption after HOT-indexed apply');

$subscriber->stop;
$publisher->stop;

done_testing();
