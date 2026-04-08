
# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# This regression test checks the behavior of the btree validation in the
# presence of missing relation segments.
#
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('test');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

# Create two tables, one with unique index and another to test
# posting list (btree duplicates).
$node->safe_psql(
	'postgres', q(
	CREATE EXTENSION amcheck;
	CREATE TABLE missingsegs_test1 AS
		SELECT * FROM generate_series(1, 3000) id;
	CREATE TABLE missingsegs_test2 AS
		SELECT 10 AS id FROM generate_series(1, 3000);
	CREATE UNIQUE INDEX bttest_unique_idx1 ON missingsegs_test1 (id);
	CREATE INDEX bttest_idx2 ON missingsegs_test2 (id);
));

my ($result, $stdout, $stderr);

# The test tables are small, so a second relation segment only exists when
# the server was built with a tiny segment size (--with-segsize-blocks).
# There is no way to simulate a lost segment otherwise; skip in that case.
my $segpath1 = relation_filepath('missingsegs_test1') . ".1";
my $segpath2 = relation_filepath('missingsegs_test2') . ".1";
if (!-e $segpath1 || !-e $segpath2)
{
	plan skip_all =>
	  'test requires small relation segment size (--with-segsize-blocks)';
}

# We have not yet broken the index, so we should get no corruption
$result = $node->safe_psql(
	'postgres', q(
	SELECT bt_index_check('bttest_unique_idx1', true, true, true);
));
is($result, '', 'run amcheck on non-broken bttest_unique_idx1');

$result = $node->safe_psql(
	'postgres', q(
	SELECT bt_index_check('bttest_idx2', true, true, true);
));
is($result, '', 'run amcheck on non-broken bttest_idx2');

# Break the relations, simulating rogue action or just fsck moving files
# into the /lost+found.
my $relpath1 = relation_filepath('missingsegs_test1');
my $relpath2 = relation_filepath('missingsegs_test2');
$node->stop;
corrupt_segment($relpath1.".1");
corrupt_segment($relpath2.".1");
$node->start;

$result = $node->safe_psql(
	'postgres', q(
		SET enable_indexscan TO off;
		SET enable_indexonlyscan TO off;
		SELECT count(id) FROM missingsegs_test1;
));
cmp_ok(
        '3000', '>', $result,
		"ensure there is missing data on missingsegs_test1");

$result = $node->safe_psql(
	'postgres', q(
		SET enable_indexscan TO off;
		SET enable_indexonlyscan TO off;
		SELECT count(id) FROM missingsegs_test2;
));
cmp_ok(
        '3000', '>', $result,
		"ensure there is missing data on missingsegs_test2");

($result, $stdout, $stderr) = $node->psql(
	'postgres', q(SELECT bt_index_check('bttest_unique_idx1', true, true, true);)
);
like(
	$stderr,
	qr/index line pointer in index "bttest_unique_idx1" points to missing page in table "missingsegs_test1"/,
	'detected corrupted segments for missingsegs_test1');

($result, $stdout, $stderr) = $node->psql(
	'postgres', q(SELECT bt_index_check('bttest_idx2', true, true, true);)
);
like(
	$stderr,
	qr/index line pointer in index "bttest_idx2" points to missing page in table "missingsegs_test2"/,
	'detected corrupted segments for missingsegs_test2');

$node->stop;
done_testing();

# Returns the filesystem path for the named relation.
sub relation_filepath
{
		my ($relname) = @_;

		my $pgdata = $node->data_dir;
		my $rel = $node->safe_psql('postgres',
			qq(SELECT pg_relation_filepath('$relname')));
		die "path not found for relation $relname" unless defined $rel;
		return "$pgdata/$rel";
}

# Rename segment so that it is in accessible
sub corrupt_segment
{
		my ($relpath) = @_;
		my $destrelpath = $relpath . ".BAK";

		rename($relpath, $destrelpath)
			or BAIL_OUT("rename failed: $!");
}

