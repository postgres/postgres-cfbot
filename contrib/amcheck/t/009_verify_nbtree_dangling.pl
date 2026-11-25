# Copyright (c) 2026, PostgreSQL Global Development Group

# An index-referenced heap slot may become LP_DEAD through pruning, but
# cannot become LP_UNUSED while bt_index_parent_check holds ShareLock.
# bt_index_check cannot make the same distinction from concurrent VACUUM.
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init(no_data_checksums => 1);
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;
$node->safe_psql('postgres', q(
	CREATE EXTENSION amcheck;
	CREATE EXTENSION pageinspect;
	CREATE TABLE dangling_unused (id int);
	INSERT INTO dangling_unused VALUES (1);
	CREATE INDEX dangling_unused_idx ON dangling_unused (id);
	CREATE TABLE dangling_offset (id int);
	INSERT INTO dangling_offset VALUES (1);
	CREATE INDEX dangling_offset_idx ON dangling_offset (id);
	CREATE TABLE dangling_dead (id int);
	INSERT INTO dangling_dead VALUES (1);
	CREATE INDEX dangling_dead_idx ON dangling_dead (id);
	DELETE FROM dangling_dead;
));
$node->safe_psql('postgres',
	'VACUUM (INDEX_CLEANUP OFF, TRUNCATE OFF) dangling_dead');
is($node->safe_psql('postgres', q(
	SELECT lp_flags FROM heap_page_items(get_raw_page('dangling_dead', 0))
	WHERE lp = 1;
)), '3', 'pruning left an LP_DEAD slot awaiting index cleanup');
is($node->safe_psql('postgres', q(
	SELECT bt_index_parent_check('dangling_dead_idx', false, false, false, true);
)), '', 'LP_DEAD is not a dangling index entry');

my %paths;
for my $table ('dangling_unused', 'dangling_offset')
{
	$paths{$table} = $node->data_dir . '/' . $node->safe_psql('postgres',
		qq(SELECT pg_relation_filepath('$table')));
}
$node->stop;

# Zero the first ItemIdData (just after the 24-byte PageHeaderData) to
# turn the indexed tuple into LP_UNUSED.  Separately, move pd_lower to
# the end of the page header so that offset 1 is beyond the ItemId array.
overwrite($paths{dangling_unused}, 24, pack('L', 0));
overwrite($paths{dangling_offset}, 12, pack('S', 24));
$node->start;

for my $case (
	['dangling_unused', qr/points to unused heap slot/],
	['dangling_offset', qr/with out-of-range offset/])
{
	my ($table, $error) = @$case;
	is($node->safe_psql('postgres',
		qq(SELECT bt_index_check('${table}_idx', false, false, true))),
		'', "bt_index_check skips $table without ShareLock");
	my ($ret, $out, $err) = $node->psql('postgres',
		qq(SELECT bt_index_parent_check('${table}_idx', false, false, false, true)));
	is($ret, 3, "bt_index_parent_check rejects $table");
	like($err, $error, "bt_index_parent_check identifies $table");
}

$node->stop;
done_testing();

sub overwrite
{
	my ($path, $offset, $bytes) = @_;
	open(my $fh, '+<', $path) or BAIL_OUT("open failed: $!");
	binmode $fh;
	defined(sysseek($fh, $offset, 0)) or BAIL_OUT("sysseek failed: $!");
	syswrite($fh, $bytes) == length($bytes)
	  or BAIL_OUT("syswrite failed: $!");
	close($fh) or BAIL_OUT("close failed: $!");
}
