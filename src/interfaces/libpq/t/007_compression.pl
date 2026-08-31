# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'Zstandard is not supported by this build'
  unless check_pg_config('#define USE_ZSTD 1');

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init(allows_streaming => 1);
$node->append_conf('postgresql.conf', 'protocol_compression = zstd');
$node->append_conf('postgresql.conf', 'summarize_wal = on');
$node->start;

my @commands = (
	'-c', 'SELECT 1',
	'-c', q{SELECT NULL::text, ''::text, E'a\nb', true, -1::bigint,
		1.25::numeric, '\x00017fff'::bytea},
	'-c', q{SELECT g, CASE WHEN g % 3 = 0 THEN NULL
		ELSE repeat(chr(64 + g), g) END FROM generate_series(1, 26) AS g},
	'-c', 'SELECT g FROM generate_series(1, 2000) AS g',
	'-c', q{SELECT repeat('x', 1024 * 1024)},
	'-c', q{DO $$ BEGIN RAISE NOTICE 'compression notice'; END $$},
	'-c', 'BEGIN',
	'-c', q{SELECT 'in transaction', repeat('y', 8192)},
	'-c', 'COMMIT',
	'-c', q{COPY (SELECT repeat('copy data ', 20) FROM generate_series(1, 100)) TO STDOUT},
	'-c', 'SELECT 2');

my ($plain_stdout, $plain_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=off', @commands ]);
my ($compressed_stdout, $compressed_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=zstd', @commands ]);
is($compressed_stdout, $plain_stdout,
	'compressed queries and COPY OUT produce the same output');
is($compressed_stderr, $plain_stderr,
	'compressed and uncompressed connections report the same notices');

my @error_commands = (
	'-c', q{SELECT 'before error', repeat('a', 8192)},
	'-c', 'SELECT 1 / 0',
	'-c', q{SELECT 'after error', repeat('b', 8192)});
my ($plain_error_stdout, $plain_error_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=off',
	  @error_commands ]);
my ($compressed_error_stdout, $compressed_error_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=zstd',
	  @error_commands ]);
is($compressed_error_stdout, $plain_error_stdout,
	'compression preserves results around an error response');
is($compressed_error_stderr, $plain_error_stderr,
	'compression preserves an error response');

my $copy_file = $node->basedir . '/copy.data';
append_to_file($copy_file,
	join('', map { "$_\tcopy data $_\n" } 1 .. 2000));
my ($copy_in_stdout, $copy_in_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=zstd',
	  '-c', 'CREATE TABLE copy_in_test (id integer, value text)',
	  '-c', "\\copy copy_in_test FROM '$copy_file'",
	  '-c', 'SELECT count(*), min(id), max(id) FROM copy_in_test' ]);
is($copy_in_stderr, '', 'compressed COPY IN produced no errors');
like($copy_in_stdout, qr/2000\|1\|2000/,
	'compressed COPY IN loaded all rows');

my $binary_file = $node->basedir . '/copy.binary';
$node->command_ok(
	[ 'psql', '-XAt', '--dbname',
	  $node->connstr('postgres') . ' compression=zstd',
	  '-c', "\\copy (SELECT g, md5(g::text) FROM generate_series(1, 2000) g) TO '$binary_file' (FORMAT binary)" ],
	'compressed binary COPY OUT');
$node->safe_psql('postgres',
	'CREATE TABLE binary_copy_test (id integer, value text)');
$node->command_ok(
	[ 'psql', '-XAt', '--dbname',
	  $node->connstr('postgres') . ' compression=zstd',
	  '-c', "\\copy binary_copy_test FROM '$binary_file' (FORMAT binary)" ],
	'compressed binary COPY IN');
is($node->safe_psql('postgres',
	'SELECT count(*), min(id), max(id), bool_and(value = md5(id::text)) FROM binary_copy_test'),
	'2000|1|2000|t', 'compressed binary COPY preserved all values');

my $pgbench_script = $node->basedir . '/compression.pgbench';
append_to_file($pgbench_script, <<'EOS');
SELECT 42 AS answer, repeat('extended result ', 5000) AS payload \gset
\if :answer != 42
  SELECT 1 / 0;
\endif
EOS
for my $query_mode ('extended', 'prepared')
{
	$node->command_ok(
		[ 'pgbench', '--no-vacuum', '--client=1', '--transactions=3',
		  '--protocol', $query_mode, '--file', $pgbench_script,
		  $node->connstr('postgres') . ' compression=zstd' ],
		"compressed $query_mode query protocol");
}

$node->command_ok(
	[ 'libpq_testclient', '--compression-reset',
	  $node->connstr('postgres') . ' compression=zstd' ],
	'PQreset discards partial compression state and starts a new stream');

$node->command_ok(
	[ 'libpq_testclient', '--compression-large-datarow',
	  $node->connstr('postgres') . ' compression=zstd' ],
	'large backend DataRow spans compressed segments');

$node->command_ok(
	[ 'libpq_testclient', '--compression-large-copy',
	  $node->connstr('postgres') . ' compression=zstd' ],
	'large frontend CopyData spans compressed segments and is traced');

$node->command_ok(
	[ 'libpq_testclient', '--compression-frame-boundaries',
	  $node->connstr('postgres') .
	  ' compression=zstd sslmode=disable gssencmode=disable' ],
	'frontend compression enforces Zstandard frame boundaries');

$node->command_ok(
	[ 'libpq_testclient', '--compression-pipeline',
	  $node->connstr('postgres') . ' compression=zstd' ],
	'compressed server responses work in libpq pipeline mode');

my $dump_file = $node->basedir . '/copy_in_test.dump';
$node->command_ok(
	[ 'pg_dump', '--format=custom', '--file', $dump_file,
	  '--table=copy_in_test', $node->connstr('postgres') ],
	'created archive for compressed pg_restore');
$node->safe_psql('postgres', 'DROP TABLE copy_in_test');
$node->command_ok(
	[ 'pg_restore', '--dbname',
	  $node->connstr('postgres') . ' compression=zstd', $dump_file ],
	'pg_restore uses compressed COPY IN');
is($node->safe_psql('postgres',
	'SELECT count(*), min(id), max(id) FROM copy_in_test'),
	'2000|1|2000', 'compressed pg_restore loaded all rows');

# An incremental backup sends its manifest with COPY, which is the only
# frontend COPY the walsender reads, and it ends the compressed frame with an
# empty CompressedData message.
my $full_backup = $node->basedir . '/full_backup';
$node->command_ok(
	[ 'pg_basebackup', '--no-sync', '--checkpoint' => 'fast',
	  '--pgdata' => $full_backup,
	  '--dbname' => $node->connstr('postgres') ],
	'full backup for a compressed UPLOAD_MANIFEST');
$node->safe_psql('postgres',
	'CREATE TABLE after_backup AS SELECT g FROM generate_series(1, 1000) g');
my $incremental_backup = $node->basedir . '/incremental_backup';
$node->command_ok(
	[ 'pg_basebackup', '--no-sync', '--checkpoint' => 'fast',
	  '--pgdata' => $incremental_backup,
	  '--incremental' => $full_backup . '/backup_manifest',
	  '--dbname' => $node->connstr('postgres') . ' compression=zstd' ],
	'incremental backup uploads its manifest over a compressed connection');

my (undef, $invalid_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=invalid',
	  '-c', 'SELECT 1' ]);
like($invalid_stderr, qr/invalid compression value: "invalid"/,
	'invalid compression value is rejected');

$node->safe_psql('postgres', "ALTER SYSTEM SET protocol_compression = 'off'");
$node->reload;
my ($preferred_stdout, $preferred_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=prefer',
	  '-c', 'SELECT 1' ]);
is($preferred_stderr, '', 'preferred compression falls back without errors');
is($preferred_stdout, '1', 'preferred compression fallback returns output');
my (undef, $disabled_stderr) = run_command(
	[ 'psql', '-XAt', '--dbname', $node->connstr('postgres') . ' compression=zstd',
	  '-c', 'SELECT 1' ]);
like($disabled_stderr, qr/does not support protocol compression method "zstd"/,
	'server can reject protocol compression');

$node->stop('fast');
done_testing();
