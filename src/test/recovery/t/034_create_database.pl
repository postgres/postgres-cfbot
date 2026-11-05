
# Copyright (c) 2023-2026, PostgreSQL Global Development Group

# Test WAL replay for CREATE DATABASE .. STRATEGY WAL_LOG.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);
$node->start;

$node->backup('backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'backup', has_streaming => 1);
$standby->start;

# This checks that any DDLs run on the template database that modify pg_class
# are persisted after creating a database from it using the WAL_LOG strategy,
# as a direct copy of the template database's pg_class is used in this case.
my $db_template = "template1";
my $db_new = "test_db_1";

# Create a table in the template database and add data then vacuum it, ensuring
# it will have both a VM and FSM.
$node->safe_psql($db_template, q{
CREATE TABLE tab_db_before_create_1 AS
  SELECT generate_series(1, 10000) AS a;
VACUUM (FREEZE, ANALYZE) tab_db_before_create_1;
});

# Copy the template database
$node->safe_psql("postgres",
	"CREATE DATABASE $db_new STRATEGY WAL_LOG TEMPLATE $db_template;");
$node->safe_psql($db_new, 'CREATE EXTENSION pageinspect;');

# Create a table after copying it. This table should persist in the template
# database but not be in the newly created copy of it.
$node->safe_psql($db_template, "CREATE TABLE tab_db_after_create_1 (a INT);");

# Flush the changes affecting the template database, then replay them.
$node->safe_psql("postgres", "CHECKPOINT;");

# Template database has the table we created after copying.
$node->stop('immediate');
$node->start;
my $result = $node->safe_psql($db_template,
	"SELECT count(*) FROM pg_class WHERE relname = 'tab_db_after_create_1';");
is($result, "1",
	"check that table exists on template after crash, with checkpoint");

# The new database should not have the table we created after copying.
$result = $node->safe_psql($db_new,
	"SELECT count(*) FROM pg_class WHERE relname = 'tab_db_after_create_1';");
is($result, "0",
	"check that there are no tables from template on new database after crash"
);

$node->wait_for_replay_catchup($standby);

# The new database should have the same data for all forks on the primary and
# standby for the table created before it was copied from the template.
for my $fork (qw(vm fsm))
{
	my $query =
	  "SELECT encode(get_raw_page('tab_db_before_create_1', '$fork', 0), 'hex');";
	my $primary_page = $node->safe_psql($db_new, $query);
	my $standby_page = $standby->safe_psql($db_new, $query);

	is($standby_page, $primary_page, "copied $fork page matches standby");
}

done_testing();
