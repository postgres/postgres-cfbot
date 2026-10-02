
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test dumping and restoring tables whose access method has options of its
# own, with and without --no-table-access-method.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE src');
$node->safe_psql(
	'src', q{
	CREATE EXTENSION dummy_table_am;
	CREATE TABLE t_mixed (a int, b text) USING dummy_table_am
		WITH (fillfactor = 70, option_int = 7, option_bool = false);
	INSERT INTO t_mixed SELECT i, 'row ' || i FROM generate_series(1, 10) i;
	CREATE TABLE t_custom (a int) USING dummy_custom_table_am
		WITH (option_a = 5);
	INSERT INTO t_custom VALUES (1), (2);
	CREATE TABLE t_heap (a int) WITH (fillfactor = 60);
	CREATE MATERIALIZED VIEW mv USING dummy_table_am
		WITH (fillfactor = 80, option_int = 3)
		AS SELECT a FROM t_mixed;
});

# Show the access method and options of each relation, and the row counts
my $describe = q{
	SELECT c.relname, a.amname, c.reloptions
		FROM pg_class c JOIN pg_am a ON a.oid = c.relam
		WHERE c.relname IN ('t_mixed', 't_custom', 't_heap', 'mv')
		ORDER BY c.relname;
	SELECT count(*) FROM t_mixed;
	SELECT count(*) FROM t_custom;
	SELECT count(*) FROM mv;
};
my $src_state = $node->safe_psql('src', $describe);

# The default AM is heap, so --no-table-access-method recreates every
# relation as heap, with only the standard options.
my $noam_state = q{mv|heap|{fillfactor=80}
t_custom|heap|
t_heap|heap|{fillfactor=60}
t_mixed|heap|{fillfactor=70}
10
2
10};

my $plain = "$tempdir/plain.sql";
my $plain_noam = "$tempdir/plain_noam.sql";
my $archive = "$tempdir/archive.dump";

$node->command_ok([ 'pg_dump', '--file' => $plain, 'src' ],
	'plain dump');
$node->command_ok(
	[
		'pg_dump', '--no-table-access-method',
		'--file' => $plain_noam, 'src'
	],
	'plain dump with --no-table-access-method');
$node->command_ok(
	[ 'pg_dump', '--format' => 'custom', '--file' => $archive, 'src' ],
	'custom-format dump');

# Standard options stay in the WITH clause, the others are set separately
my $dump = slurp_file($plain);
like(
	$dump,
	qr/CREATE TABLE public\.t_mixed \(.*?\)\nWITH \(fillfactor='70'\);/s,
	'standard options of a non-heap table are in CREATE TABLE');
like(
	$dump,
	qr/^ALTER TABLE ONLY public\.t_mixed SET \(option_int='7', option_bool='false'\);$/m,
	'AM-specific options of a table are set by ALTER TABLE');
like(
	$dump,
	qr/^ALTER MATERIALIZED VIEW public\.mv SET \(option_int='3'\);$/m,
	'AM-specific options of a matview are set by ALTER MATERIALIZED VIEW');
like(
	$dump,
	qr/^ALTER TABLE ONLY public\.t_custom SET \(option_a='5'\);$/m,
	'table with only AM-specific options');
like(
	$dump,
	qr/CREATE TABLE public\.t_heap \(.*?\)\nWITH \(fillfactor='60'\);/s,
	'heap table options are in CREATE TABLE');
unlike($dump, qr/ALTER TABLE ONLY public\.t_heap SET/,
	'heap table options are not set separately');

unlike(slurp_file($plain_noam), qr/TABLE AM OPTIONS|option_int|option_a/,
	'--no-table-access-method leaves out AM-specific options');

my @psql = ('psql', '--no-psqlrc', '--set' => 'ON_ERROR_STOP=1');

$node->safe_psql('postgres', 'CREATE DATABASE dst_plain');
$node->command_ok([ @psql, '--file' => $plain, '--dbname' => 'dst_plain' ],
	'restore plain dump');
is($node->safe_psql('dst_plain', $describe),
	$src_state, 'plain dump restores the same options');

$node->safe_psql('postgres', 'CREATE DATABASE dst_plain_noam');
$node->command_ok(
	[ @psql, '--file' => $plain_noam, '--dbname' => 'dst_plain_noam' ],
	'restore plain dump made with --no-table-access-method');
is($node->safe_psql('dst_plain_noam', $describe),
	$noam_state,
	'plain dump made with --no-table-access-method restores tables and data'
);

$node->safe_psql('postgres', 'CREATE DATABASE dst_archive');
$node->command_ok(
	[
		'pg_restore', '--exit-on-error',
		'--dbname' => 'dst_archive', $archive
	],
	'restore custom-format dump');
is($node->safe_psql('dst_archive', $describe),
	$src_state, 'custom-format dump restores the same options');

$node->safe_psql('postgres', 'CREATE DATABASE dst_archive_noam');
$node->command_ok(
	[
		'pg_restore', '--exit-on-error', '--no-table-access-method',
		'--dbname' => 'dst_archive_noam', $archive
	],
	'pg_restore --no-table-access-method');
is($node->safe_psql('dst_archive_noam', $describe),
	$noam_state,
	'pg_restore --no-table-access-method restores tables and data');

# Selecting a table also selects its AM-specific options
$node->safe_psql('postgres', 'CREATE DATABASE dst_table');
$node->safe_psql('dst_table', 'CREATE EXTENSION dummy_table_am');
$node->command_ok(
	[
		'pg_restore', '--exit-on-error',
		'--table' => 't_mixed',
		'--dbname' => 'dst_table', $archive
	],
	'pg_restore --table');
is( $node->safe_psql(
		'dst_table', q{SELECT reloptions FROM pg_class WHERE relname = 't_mixed'}
	),
	'{fillfactor=70,option_int=7,option_bool=false}',
	'pg_restore --table restores AM-specific options');

$node->stop;

done_testing();
