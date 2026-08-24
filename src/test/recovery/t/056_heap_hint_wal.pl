# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
command_ok(
	[ 'pg_checksums', '--disable', '-D', $node->data_dir ],
	'disabled data checksums');
$node->append_conf(
	'postgresql.conf', qq(
full_page_writes = on
wal_log_hints = on
bgwriter_lru_maxpages = 0
checkpoint_timeout = '1h'
));
$node->start;

$node->safe_psql('postgres',
	'CREATE EXTENSION pageinspect; '
	. 'CREATE TABLE hints AS SELECT g FROM generate_series(1, 100) g');

# Ensure that the heap page is clean and no longer in shared buffers.  The
# first scan after the restart will therefore WAL-log its visibility hints.
$node->restart;

my $start_lsn = $node->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
my $page_lsn = $node->safe_psql('postgres',
	q[SELECT lsn FROM page_header(get_raw_page('hints', 0))]);
is($node->safe_psql('postgres', 'SELECT count(*) FROM hints'),
	'100', 'scanned all tuples');
is($node->safe_psql('postgres',
	q[SELECT lsn FROM page_header(get_raw_page('hints', 0))]),
	$page_lsn, 'heap hint WAL does not advance the page LSN');
my $end_lsn = $node->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
my $relfilenode = $node->safe_psql('postgres',
	q[SELECT pg_relation_filenode('hints'::regclass)]);

# Make the WAL segment available to pg_waldump.
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

my ($stdout, $stderr) = run_command(
	[
		'pg_waldump', '-p', $node->data_dir,
		'-s', $start_lsn, '-e', $end_lsn,
		'-r', 'HeapHint', '-b'
	]);

is($stderr, '', 'pg_waldump produced no diagnostics');
like(
	$stdout,
	qr/desc: HINT ntuples: 100\n\s+blkref #0: rel \d+\/\d+\/$relfilenode fork main blk 0/,
	'heap visibility hints use a compact WAL record');
unlike(
	$stdout,
	qr/\(FPW\)/,
	'heap hint WAL record has no full-page image');

# Exercise replay of the heap hint WAL record.
$node->stop('immediate');
$node->start;
is($node->safe_psql('postgres', q[
	SELECT bool_and((t_infomask & 2304) = 2304)
	FROM heap_page_items(get_raw_page('hints', 0))]),
	't', 'replay restored heap visibility hint bits');
is($node->safe_psql('postgres', 'SELECT count(*) FROM hints'),
	'100', 'table is readable after replaying heap hint WAL');

$node->stop;

command_ok(
	[ 'pg_checksums', '--enable', '-D', $node->data_dir ],
	'enabled data checksums');
$node->start;
$node->safe_psql('postgres',
	'CREATE TABLE checksum_hints AS SELECT g FROM generate_series(1, 100) g');
$node->restart;

$start_lsn = $node->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
is($node->safe_psql('postgres', 'SELECT count(*) FROM checksum_hints'),
	'100', 'scanned all tuples on a checksummed page');
$end_lsn = $node->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
$relfilenode = $node->safe_psql('postgres',
	q[SELECT pg_relation_filenode('checksum_hints'::regclass)]);
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');

($stdout, $stderr) = run_command(
	[
		'pg_waldump', '-p', $node->data_dir,
		'-s', $start_lsn, '-e', $end_lsn,
		'-r', 'XLOG', '-b'
	]);

is($stderr, '', 'pg_waldump produced no checksum diagnostics');
like(
	$stdout,
	qr/desc: FPI_FOR_HINT\s*\n\s+blkref #0: rel \d+\/\d+\/$relfilenode fork main blk 0 \(FPW\)/,
	'checksummed heap hint update uses a full-page image');

$node->stop;

done_testing();
