# Copyright (c) 2025-2026, PostgreSQL Global Development Group

# Test custom pgstats functionality
#
# This script includes tests for both variable and fixed-sized custom
# pgstats:
# - Creation, updates, and reporting.
# - Persistence across restarts.
# - Loss after crash recovery.
# - Resets for fixed-sized stats.
# - Custom hash tables for variable-sized stats

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Copy;

my $result;
my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_custom_var_stats, test_custom_fixed_stats'"
);
$node->start;

$node->safe_psql('postgres', q(CREATE EXTENSION test_custom_var_stats));
$node->safe_psql('postgres', q(CREATE EXTENSION test_custom_fixed_stats));

# Verify custom stats kinds appear in pg_stat_kind_info.
$result = $node->safe_psql(
	'postgres',
	q(SELECT id, name, builtin, fixed_amount, accessed_across_databases,
	         write_to_file, own_hash
	    FROM pg_stat_kind_info
	    WHERE name LIKE 'test_custom%' ORDER BY id));
is( $result,
	qq{25|test_custom_var_stats|f|f|t|t|f
26|test_custom_fixed_stats|f|t|f|t|f},
	"custom stats kinds visible in pg_stat_kind_info");

my @var_stats_kinds = (
	{
		name => 'test_custom_var_stats_own_hash',
		own_hash => 't',
	},
	{
		name => 'test_custom_var_stats',
		own_hash => 'f',
	});

# Initialize, to adjust later.
$node->append_conf('postgresql.conf',
	"test_custom_var_stats.kind = 'test_custom_var_stats'");

# Test variable-sized stats with both dshash configurations.
foreach my $stats_kind (@var_stats_kinds)
{
	my $kind_name = $stats_kind->{name};
	my $own_hash = $stats_kind->{own_hash};

	# Restart with the appropriate setting.
	$node->adjust_conf('postgresql.conf', 'test_custom_var_stats.kind',
		"'$kind_name'");
	$node->restart;

	$result = $node->safe_psql('postgres',
		"SELECT own_hash FROM pg_stat_kind_info WHERE name = '$kind_name'");
	is($result, $own_hash, "dedicated hash reported for $kind_name");

	# Create entries for variable-sized stats.
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry1', 'Test entry 1')));
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry2', 'Test entry 2')));
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry3', 'Test entry 3')));
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry4', 'Test entry 4')));

	# Update counters: entry1=2, entry2=3, entry3=2, entry4=3
	$node->safe_psql(
		'postgres',
		q(select test_custom_stats_var_update(name)
		  from (values ('entry1', 2), ('entry2', 3),
		               ('entry3', 2), ('entry4', 3)) as stats(name, n),
		       generate_series(1, n);
		  select pg_stat_force_next_flush()));

	# Test data reports.
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry1')));
	is( $result,
		"entry1|2|Test entry 1",
		"variable-sized stats of entry1 for $kind_name");

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry2')));
	is( $result,
		"entry2|3|Test entry 2",
		"variable-sized stats of entry2 for $kind_name");

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry3')));
	is( $result,
		"entry3|2|Test entry 3",
		"variable-sized stats of entry3 for $kind_name");

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry4')));
	is( $result,
		"entry4|3|Test entry 4",
		"variable-sized stats of entry4 for $kind_name");

	# Test drop of variable-sized stats via pgstat_drop_entry().
	$node->safe_psql('postgres',
		q(select * from test_custom_stats_var_drop('entry3')));
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry3')));
	is($result, "", "entry3 not found after drop for $kind_name");

	# In the shared hash case, keep a relation stats entry of a different kind
	# around to exercise filtering by kind before the callback is called.
	if ($own_hash eq 'f')
	{
		$node->safe_psql('postgres',
			q(CREATE TABLE test_custom_stats_shared_hash_rel(a int);
			  INSERT INTO test_custom_stats_shared_hash_rel VALUES (1);
			  SELECT pg_stat_force_next_flush()));
		$result = $node->safe_psql('postgres',
			q(SELECT n_tup_ins FROM pg_stat_user_tables
			  WHERE relname = 'test_custom_stats_shared_hash_rel'));
		is($result, "1", "relation stats entry exists in shared hash for $kind_name");
	}

	# Test drop of variable-sized stats via pgstat_drop_matching_entries().
	$node->safe_psql('postgres',
		q(select * from test_custom_stats_var_drop_matching('entry4')));
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry4')));
	is($result, "", "entry4 not found after drop for $kind_name");
	if ($own_hash eq 'f')
	{
		# The relation stats entry should survive the custom stats drop.
		$result = $node->safe_psql('postgres',
			q(SELECT n_tup_ins FROM pg_stat_user_tables
			  WHERE relname = 'test_custom_stats_shared_hash_rel'));
		is($result, "1",
			"relation stats entry remains after custom stats drop for $kind_name");
	}

	# Test persistence across clean restart.
	$node->stop();
	$node->start();

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry1')));
	is( $result,
		"entry1|2|Test entry 1",
		"variable-sized stats of entry1 persist after clean restart for $kind_name"
	);

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry2')));
	is( $result,
		"entry2|3|Test entry 2",
		"variable-sized stats of entry2 persist after clean restart for $kind_name"
	);

	# Test reset of variable-sized stats.
	$node->safe_psql('postgres', q(select test_custom_stats_var_reset()));
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry1')));
	is( $result,
		"entry1|0|Test entry 1",
		"variable-sized stats of entry1 reset for $kind_name");

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry2')));
	is( $result,
		"entry2|0|Test entry 2",
		"variable-sized stats of entry2 reset for $kind_name");

	# Test loss after crash recovery.
	$node->stop('immediate');
	$node->start;

	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry1')));
	is($result, "",
		"variable-sized stats of entry1 lost after crash recovery for $kind_name"
	);
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry2')));
	is($result, "",
		"variable-sized stats of entry2 lost after crash recovery for $kind_name"
	);

	# Test drop of custom variable-numbered stats via
	# pgstat_drop_matching_entries() with an all-kinds scan and a callback.
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry5', 'Test entry 5')));
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry6', 'Test entry 6')));
	my $matching_all_rel = "test_custom_stats_matching_all_rel_$own_hash";
	$node->safe_psql('postgres',
		"CREATE TABLE $matching_all_rel(a int);
		 INSERT INTO $matching_all_rel VALUES (1);
		 SELECT pg_stat_force_next_flush()");
	$result = $node->safe_psql('postgres',
		"SELECT n_tup_ins FROM pg_stat_user_tables
		 WHERE relname = '$matching_all_rel'");
	is($result, "1",
		"relation stats entry exists before all-kinds matching drop for $kind_name");
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_drop_matching('entry5', true)));
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry5')));
	is($result, "",
		"entry5 not found after all-kinds matching drop for $kind_name");
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry6')));
	is($result, "entry6|0|Test entry 6",
		"entry6 remains after all-kinds matching drop for $kind_name");
	$result = $node->safe_psql('postgres',
		"SELECT n_tup_ins FROM pg_stat_user_tables
		 WHERE relname = '$matching_all_rel'");
	is($result, "1",
		"relation stats entry remains after all-kinds matching drop for $kind_name");

	# Test drop of all custom variable-numbered stats entries via
	# pgstat_drop_matching_entries() with a NULL callback.
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry5', 'Test entry 5')));
	$node->safe_psql('postgres',
		q(select test_custom_stats_var_create('entry6', 'Test entry 6')));
	if ($own_hash eq 'f')
	{
		$node->safe_psql('postgres',
			q(INSERT INTO test_custom_stats_shared_hash_rel VALUES (2);
			  SELECT pg_stat_force_next_flush()));
		$result = $node->safe_psql('postgres',
			q(SELECT n_tup_ins FROM pg_stat_user_tables
			  WHERE relname = 'test_custom_stats_shared_hash_rel'));
		is($result, "1",
			"relation stats entry exists before kind drop all for $kind_name");
	}
	$node->safe_psql('postgres', q(select test_custom_stats_var_drop_all()));
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry5')));
	is($result, "", "entry5 not found after drop all for $kind_name");
	$result = $node->safe_psql('postgres',
		q(select * from test_custom_stats_var_report('entry6')));
	is($result, "", "entry6 not found after drop all for $kind_name");
	if ($own_hash eq 'f')
	{
		$result = $node->safe_psql('postgres',
			q(SELECT n_tup_ins FROM pg_stat_user_tables
			  WHERE relname = 'test_custom_stats_shared_hash_rel'));
		is($result, "1",
			"relation stats entry remains after kind drop all for $kind_name");
	}
}

# fixed-sized stats are updated 3 times, so the report below should match.
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));

$result = $node->safe_psql('postgres',
	q(select numcalls from test_custom_stats_fixed_report()));
is($result, "3", "report for fixed-sized stats");

# Test persistence of fixed-sized stats across clean restart.
$node->stop();
$node->start();

$result = $node->safe_psql('postgres',
	q(select numcalls from test_custom_stats_fixed_report()));
is($result, "3", "fixed-sized stats persist after clean restart");

# Test fixed-sized stats after crash recovery.
$node->stop('immediate');
$node->start;

# Crash recovery sets the reset timestamp.
$result = $node->safe_psql('postgres',
	q(select numcalls from test_custom_stats_fixed_report() where stats_reset is not null)
);
is($result, "0", "fixed-sized stats are reset after crash recovery");

# Test reset of fixed-sized stats.
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));
$node->safe_psql('postgres', q(select test_custom_stats_fixed_update()));

$result = $node->safe_psql('postgres',
	q(select numcalls from test_custom_stats_fixed_report()));
is($result, "3", "report of fixed-sized before manual reset");

$node->safe_psql('postgres', q(select test_custom_stats_fixed_reset()));

$result = $node->safe_psql('postgres',
	q(select numcalls from test_custom_stats_fixed_report() where stats_reset is not null)
);
is($result, "0", "report of fixed-sized after manual reset");

# Test completed successfully
done_testing();
