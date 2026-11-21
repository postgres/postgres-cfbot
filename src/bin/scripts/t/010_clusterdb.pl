
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

program_help_ok('clusterdb');
program_version_ok('clusterdb');
program_options_handling_ok('clusterdb');

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->issues_sql_like(
	['clusterdb'],
	qr/statement: CLUSTER;/,
	'SQL CLUSTER run');

$node->command_fails_like(
	[ 'clusterdb', '--table' => 'nonexistent' ],
	qr/relation "nonexistent" does not exist/,
	'fails with nonexistent table');

$node->safe_psql('postgres',
	'CREATE TABLE test1 (a int); CREATE INDEX test1x ON test1 (a); CLUSTER test1 USING test1x'
);
$node->issues_sql_like(
	[ 'clusterdb', '--table' => 'test1' ],
	qr/statement: CLUSTER public\.test1;/,
	'cluster specific table');

$node->command_ok([qw(clusterdb --echo --verbose dbname=template1)],
	'clusterdb with connection string');

# A database that cannot be connected to terminates --all, unless --continue
# is given.  This needs its own cluster, since connections can only be refused
# to a role that is not a superuser.
my $cnode = PostgreSQL::Test::Cluster->new('continue');
$cnode->init(auth_extra => [ '--create-role' => 'regress_continue' ]);
$cnode->start;
$cnode->safe_psql('postgres',
	'CREATE ROLE regress_continue LOGIN IN ROLE pg_maintain');
$cnode->safe_psql('postgres', 'CREATE DATABASE regress_noconn');
$cnode->safe_psql('postgres',
	'REVOKE CONNECT ON DATABASE regress_noconn FROM PUBLIC');

$cnode->command_fails_like(
	[ 'clusterdb', '--all', '--username' => 'regress_continue' ],
	qr/permission denied for database "regress_noconn"/,
	'--all fails on a database that cannot be connected to');
$cnode->command_fails_like(
	[ 'clusterdb', '--continue', 'postgres' ],
	qr/cannot use the "continue" option without "all"/,
	'--continue requires --all');
$cnode->command_checks_all(
	[ 'clusterdb', '--all', '--continue', '--username' => 'regress_continue' ],
	0,
	[qr/clustering database "template1"/],
	[qr/warning: skipping database "regress_noconn": /],
	'--all --continue skips databases that cannot be connected to');

done_testing();
