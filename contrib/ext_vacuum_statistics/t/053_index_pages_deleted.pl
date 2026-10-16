# Copyright (c) 2026, PostgreSQL Global Development Group

# Count pages newly deleted by each vacuum, without recounting pages that
# were already deleted. Exercise serial and parallel bulkdelete and cleanup.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('index_pages');
$node->init;
$node->append_conf('postgresql.conf', q{
shared_preload_libraries = 'ext_vacuum_statistics'
autovacuum = off
max_parallel_maintenance_workers = 1
min_parallel_index_scan_size = 0
});
$node->start;
$node->safe_psql('postgres', q{
CREATE EXTENSION ext_vacuum_statistics;
CREATE TABLE pages_test (id integer, other integer)
  WITH (autovacuum_enabled = off);
CREATE INDEX pages_test_id ON pages_test (id);
CREATE INDEX pages_test_other ON pages_test (other);
});

for my $parallel (0, 1)
{
	$node->safe_psql('postgres', q{
TRUNCATE pages_test;
INSERT INTO pages_test SELECT i, -i FROM generate_series(1, 100000) i;
DELETE FROM pages_test WHERE id < 90000;
});

	for my $pass (1, 2)
	{
		$node->safe_psql('postgres',
			'SELECT ext_vacuum_statistics.vacuum_statistics_reset()');
		my ($stdout, $stderr) = ('', '');
		my $label = "parallel=$parallel, pass=$pass";
		is($node->psql('postgres', qq{
VACUUM (VERBOSE, DISABLE_PAGE_SKIPPING, INDEX_CLEANUP ON,
        PARALLEL $parallel, TRUNCATE OFF) pages_test;
}, stdout => \$stdout, stderr => \$stderr), 0, "$label vacuum succeeds");
		if ($parallel)
		{
			like($stderr, qr/launched 1 parallel vacuum worker/,
				"$label uses a parallel worker");
		}

		for my $index ('pages_test_id', 'pages_test_other')
		{
			my ($newly_deleted, $currently_deleted) = $stderr =~
			  /index "$index":.*?pages: \d+ in total, (\d+) newly deleted, (\d+) currently deleted/;
			ok(defined $newly_deleted, "$label $index instrumentation available");
			cmp_ok($currently_deleted, '>', 0,
				"$label $index has deleted pages");
			if ($pass == 1)
			{
				cmp_ok($newly_deleted, '>', 0,
					"$label $index deletes new pages");
			}
			else
			{
				is($newly_deleted, '0',
					"$label $index deletes no additional pages");
			}
			is($node->safe_psql('postgres', qq{
SELECT pages_deleted FROM ext_vacuum_statistics.pg_stats_vacuum_indexes
WHERE indexrelname = '$index';
}), $newly_deleted, "$label $index reports only newly deleted pages");
		}
	}
}

$node->stop;
done_testing();
