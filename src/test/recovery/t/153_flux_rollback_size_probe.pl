# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Rolling back a large FLUX UPDATE must not probe the relation's size once
# per undo record.
#
# Before each undo record is applied, flux_undo_apply() checks that the
# record's block still exists.  It used to probe the relation's size for
# every record.  A size probe seeks the relation's last segment file, and
# when the relcache entry does not survive between records (as under
# RELCACHE_FORCE_RELEASE, or after invalidations) the storage manager is
# reopened and every segment file is visited again.  A rollback then cost
# O(records x segments): with 6-block segments, rolling back a 100,000-row
# UPDATE took over two minutes instead of a few seconds.
#
# While a backend rolls back its own transaction, it still holds the lock
# its changes took, so the relation cannot shrink underneath it: the size
# needs to be re-read only for a block past the last size seen.  The
# injection point flux-undo-size-probe fires on each size probe; count
# them.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_rollback_size');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
shared_preload_libraries = 'injection_points'
));
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'injection_points not installed';
}

$node->safe_psql(
	'postgres', q{
CREATE EXTENSION injection_points;
CREATE TABLE f (id int, v text) USING flux;
INSERT INTO f SELECT g, 'x' || g FROM generate_series(1, 2000) g;
});

my $nblocks = $node->safe_psql('postgres',
	"SELECT pg_relation_size('f') / current_setting('block_size')::int");
cmp_ok($nblocks, '>', 4, 'the table spans several blocks');

# Each size probe raises a NOTICE; count them in the rolled-back session.
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', q{
SELECT injection_points_set_local();
SELECT injection_points_attach('flux-undo-size-probe', 'notice');
BEGIN;
UPDATE f SET v = v || 'u';
ROLLBACK;
SELECT count(*) FROM f WHERE v LIKE '%u';
});
is($ret, 0, 'rollback succeeded');
is((split /\n/, $stdout)[-1], '0', 'every row was rolled back');

my $probes = () = $stderr =~
  /NOTICE:  notice triggered for injection point flux-undo-size-probe/g;

# One undo record per row; the rows are on $nblocks blocks.  Probing once
# per record would give about 2000.  With the size remembered, a probe is
# needed only the first time, and again only if the table grew.
cmp_ok($probes, '>=', 1, 'the size is probed');
cmp_ok($probes, '<=', 3,
	"the size is probed at most a few times for 2000 records (got $probes)");

$node->stop;
done_testing();
