
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $dbname1 = 'regression_src';
my $dbname2 = 'regression_dest1';
my $dbname3 = 'regression_dest2';
my $dbname4 = 'regression_dest3';
my $dbname5 = 'regression_dest4';

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $backupdir = $node->backup_dir;

$node->run_log([ 'createdb', $dbname1 ]);
$node->run_log([ 'createdb', $dbname2 ]);
$node->run_log([ 'createdb', $dbname3 ]);
$node->run_log([ 'createdb', $dbname4 ]);
$node->run_log([ 'createdb', $dbname5 ]);

$node->safe_psql(
	$dbname1,
	qq{
create type digit as enum ('0', '1', '2', '3', '4', '5', '6', '7', '8', '9');

-- plain table with index
create table tplain (en digit, data int unique);
insert into tplain select (x%10)::text::digit, x from generate_series(1,1000) x;

-- non-troublesome hashed partitioning
create table ths (mod int, data int, unique(mod, data)) partition by hash(mod);
create table ths_p1 partition of ths for values with (modulus 3, remainder 0);
create table ths_p2 partition of ths for values with (modulus 3, remainder 1);
create table ths_p3 partition of ths for values with (modulus 3, remainder 2);
insert into ths select (x%10), x from generate_series(1,1000) x;

-- dangerous hashed partitioning
create table tht (en digit, data int, unique(en, data)) partition by hash(en);
create table tht_p1 partition of tht for values with (modulus 3, remainder 0);
create table tht_p2 partition of tht for values with (modulus 3, remainder 1);
create table tht_p3 partition of tht for values with (modulus 3, remainder 2);
insert into tht select (x%10)::text::digit, x from generate_series(1,1000) x;
	});

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump1",
		$node->connstr($dbname1),
	],
	'parallel dump');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname2),
		'--jobs' => 3,
		"$backupdir/dump1",
	],
	'parallel restore');

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump2",
		'--inserts',
		$node->connstr($dbname1),
	],
	'parallel dump as inserts');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname3),
		'--jobs' => 3,
		"$backupdir/dump2",
	],
	'parallel restore as inserts');

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--max-table-segment-pages' => 2,
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump3",
		$node->connstr($dbname1),
	],
	'parallel dump with chunks of two heap pages');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname4),
		'--jobs' => 3,
		"$backupdir/dump3",
	],
	'parallel restore with chunks of two heap pages');

my $table = 'tplain';
my $tablehash_query = "SELECT '$table', sum(hashtext(t::text)), count(*) FROM $table AS t";

my $result_1 = $node->safe_psql($dbname1, $tablehash_query);
my $result_4 = $node->safe_psql($dbname4, $tablehash_query);

is($result_4, $result_1, "Hash check for $table: restored db ($result_4) vs original db ($result_1)");

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--max-table-segment-pages' => 2,
		'--inserts',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump4",
		$node->connstr($dbname1),
	],
	'parallel dump with chunks of two heap pages using inserts');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname5),
		'--jobs' => 3,
		"$backupdir/dump4",
	],
	'parallel restore with chunks of two heap pages using inserts');

my $result_5 = $node->safe_psql($dbname5, $tablehash_query);
is($result_5, $result_1, "Hash check for $table (inserts): restored db ($result_5) vs original db ($result_1)");

done_testing();
