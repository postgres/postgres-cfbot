
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test suite for testing enabling data checksums in an online cluster with
# global temporary tables
use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use DataChecksums::Utils;

# Initialize node with checksums disabled.
my $node = PostgreSQL::Test::Cluster->new('global_temp_table_node');
$node->init(no_data_checksums => 1);
$node->start;

# Create a global temporary table in an interactive psql process.  Should act
# as a barrier for checksum enablement to block on.
my $bsession = $node->background_psql('postgres');
$bsession->query_safe(
	'CREATE GLOBAL TEMPORARY TABLE gtt AS SELECT * FROM generate_series(1, 10000) x;');

# Ensure that checksums are disabled
test_checksum_state($node, 'off');

# In another session, make sure we can see the blocking global temporary table
# but start processing anyways and check that we are blocked with a proper
# wait event.
my $result = $node->safe_psql('postgres',
	"SELECT relpersistence FROM pg_catalog.pg_class WHERE relname = 'gtt';");
is($result, 'g', 'ensure we can see the global temporary table');

# Enable, but stop waiting at inprogress-on since it will sit there until the
# above temporary table is removed.
enable_data_checksums($node, wait => 'inprogress-on');

# Ensure that checksum enablement continues to block
sleep(1);
test_checksum_state($node, 'inprogress-on');

# Make sure background session can still read back its data
$result = $bsession->query_safe('SELECT count(*) FROM gtt WHERE x % 2 = 0;');
is($result, '5000', 'ensure global temporary table can still be read');

# Quit background session and check that checksum enablement unblocks
$bsession->quit;
wait_for_checksum_state($node, 'on');

$node->stop;
done_testing();
