
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Use perl one-liner as a portable 'cat' replacement for Windows compatibility.
# On Windows, perl opens file handles in text mode by default, which corrupts
# binary archive data by translating newlines and interpreting EOF characters.
# We use -Mopen=IO,:raw to force raw binary mode. We use -pe 1 instead of
# -pe '' to avoid shell quoting issues with empty strings on Windows cmd.exe.
my $perlbin = $^X;
$perlbin =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;
my $perl_cat = "\"$perlbin\" -Mopen=IO,:raw -pe 1";

my $dbname1 = 'regression_src';
my $dbname2 = 'regression_dest1';
my $dbname3 = 'regression_dest2';
my $dbname4 = 'regression_dest3';
my $dbname5 = 'regression_dest_warning';

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $backupdir = $node->backup_dir;
$backupdir =~ s!\\!/!g if $PostgreSQL::Test::Utils::windows_os;

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

my $is_win = $PostgreSQL::Test::Utils::windows_os;

unless ($is_win)
{
	mkdir "$backupdir/dump_pipe";

	my $pipe_dump = "$perl_cat > \"$backupdir/dump_pipe/%f\"";
	my $pipe_restore = "$perl_cat \"$backupdir/dump_pipe/%f\"";

	$node->command_ok(
		[
			'pg_dump',
			'--format' => 'directory',
			'--no-sync',
			'--jobs' => 2,
			"--pipe=$pipe_dump",
			$node->connstr($dbname1),
		],
		'parallel dump with pipe');

	$node->command_ok(
		[
			'pg_restore', '--verbose',
			'--dbname' => $node->connstr($dbname4),
			'--format' => 'directory',
			'--jobs' => 3,
			"--pipe=$pipe_restore",
		],
		'parallel restore with pipe');

	# Test warning when parallel jobs are used without %f in pipe command.
	# Use a simple command that doesn't use %f.
	my $pipe_no_f = "$perl_cat > \"$backupdir/no_f.out\"";
	$node->command_checks_all(
		[
			'pg_dump',
			'--format' => 'directory',
			'--no-sync',
			'--jobs' => 2,
			"--pipe=$pipe_no_f",
			$node->connstr($dbname1),
		],
		0,
		[],
		[qr/parallel jobs with --pipe usually require the "%f" placeholder/],
		'parallel dump without %f issues warning');

	# Test warning in pg_restore when parallel jobs are used without %f.
	# We restore into a clean database ($dbname5) using --schema-only to prevent
	# pg_restore from attempting to read table data files. We configure the pipe
	# command to cat the TOC file directly (lacking %f), which allows pg_restore
	# to successfully read and restore the metadata and exit with 0, while still
	# emitting the warning on stderr.
	my $pipe_restore_no_f_success = "$perl_cat \"$backupdir/dump_pipe/toc.dat\"";

	$node->command_checks_all(
		[
			'pg_restore',
			'--format' => 'directory',
			'--jobs' => 2,
			'--schema-only',
			"--pipe=$pipe_restore_no_f_success",
			'--dbname' => $node->connstr($dbname5),
		],
		0,
		[],
		[qr/parallel jobs with --pipe usually require the "%f" placeholder/],
		'parallel restore without %f issues warning and succeeds');
}

done_testing();

