# Copyright (c) 2026, PostgreSQL Global Development Group

# Crash recovery must replay GiST page splits, page deletion, page recycling
# and page reuse correctly, leaving the index consistent with the heap.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;

# Keep every GiST record after our checkpoint inside the recovery range, and
# check each replayed page against its full-page image.
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
checkpoint_timeout = 1h
max_wal_size = 4GB
wal_consistency_checking = gist
});
$node->start;

$node->safe_psql(
	'postgres', q{
CREATE TABLE gist_crash (id int4, p point);
CREATE INDEX gist_crash_idx ON gist_crash USING gist (p);
});

# Everything from here on is replayed by crash recovery.
$node->safe_psql('postgres', 'CHECKPOINT');

my $where = 'p <@ box(point(0,0), point(1000000,1000000))';

# Return the same query's result through an index scan and a sequential scan.
sub checksums
{
	my ($node) = @_;
	my $agg = "count(*) || ' ' || coalesce(md5(string_agg(id::text, ',' ORDER BY id)), 'empty')";
	my $idx = $node->safe_psql('postgres',
		"SET enable_seqscan = off; SET enable_bitmapscan = off;
		 SELECT $agg FROM gist_crash WHERE $where");
	my $seq = $node->safe_psql('postgres',
		"SET enable_indexscan = off; SET enable_bitmapscan = off;
		 SET enable_indexonlyscan = off;
		 SELECT $agg FROM gist_crash WHERE $where");
	return ($idx, $seq);
}

# Page splits while growing the index.
$node->safe_psql('postgres',
	'INSERT INTO gist_crash SELECT g, point(g*10, g*10) FROM generate_series(1, 20000) g'
);

# Remove tuples from leaf pages that stay in use, then empty a range of leaf
# pages so that they get deleted.
$node->safe_psql('postgres', 'DELETE FROM gist_crash WHERE id % 2 = 1');
$node->safe_psql('postgres', 'DELETE FROM gist_crash WHERE id > 5000');
$node->safe_psql('postgres', 'VACUUM gist_crash');

# Move the deleted pages behind the xid horizon, recycle them, then reuse them.
$node->safe_psql('postgres', 'SELECT pg_current_xact_id()');
$node->safe_psql('postgres', 'VACUUM gist_crash');
$node->safe_psql('postgres',
	'INSERT INTO gist_crash SELECT g, point(g*10, g*10) FROM generate_series(5001, 8000) g'
);

my ($idx_before, $seq_before) = checksums($node);
is($idx_before, $seq_before,
	'index scan matches sequential scan before crash');

$node->stop('immediate');
$node->start;

my $log = slurp_file($node->logfile);
like($log, qr/redo starts at/, 'crash recovery replayed WAL');
unlike($log, qr/inconsistent page found/,
	'no WAL consistency check failure during replay');

my ($idx_after, $seq_after) = checksums($node);
is($idx_after, $idx_before, 'index scan unchanged by crash recovery');
is($idx_after, $seq_after,
	'index scan matches sequential scan after crash recovery');

# The recovered index must still be usable through another round of page
# deletion and reuse.
$node->safe_psql('postgres', 'DELETE FROM gist_crash');
$node->safe_psql('postgres', 'VACUUM gist_crash');
$node->safe_psql('postgres',
	'INSERT INTO gist_crash SELECT g, point(g*10, g*10) FROM generate_series(1, 3000) g'
);
my ($idx_final, $seq_final) = checksums($node);
is($idx_final, $seq_final,
	'index scan matches sequential scan after reuse following recovery');

$node->stop;
done_testing();
