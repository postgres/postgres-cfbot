
# Copyright (c) 2026, PostgreSQL Global Development Group

# Check the whole sequence of values that commands report through the
# pg_stat_progress_* views.
#
# A server compiled with PROGRESS_DEBUG logs every change to a backend's
# progress state.  This test runs a series of commands, reads those lines
# back from the server log and checks them: first against rules that hold
# for every command (see ProgressCheck.pm), then against the exact
# succession of phases and the final values expected for each command.
#
# The first check, that ProgressCheck.pm describes every macro in
# commands/progress.h, runs on any build.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use ProgressCheck;

my $progress_h = $ENV{PROGRESS_H};
die "PROGRESS_H is not set" unless defined $progress_h;

my $spec = ProgressCheck::load_spec($progress_h);
my @problems = ProgressCheck::check_spec($spec);
is_deeply(\@problems, [],
	'ProgressCheck.pm describes every macro of commands/progress.h')
  or diag(join("\n", @problems));

# Is the server compiled with PROGRESS_DEBUG?  It is set in the compiler
# flags, as a buildfarm animal would (CPPFLAGS or CFLAGS with configure,
# c_args with meson), or in pg_config_manual.h.  Find out without starting
# a server, so that the test costs nothing in other builds.
my ($cppflags) = run_command([ 'pg_config', '--cppflags' ]);
my ($cflags) = run_command([ 'pg_config', '--cflags' ]);
(my $manual_h = $progress_h) =~ s{commands/progress\.h$}{pg_config_manual.h};
if ("$cppflags $cflags" !~ /-DPROGRESS_DEBUG\b/
	&& slurp_file($manual_h) !~ /^\s*#\s*define\s+PROGRESS_DEBUG\b/m)
{
	note 'not compiled with PROGRESS_DEBUG, skipping the traces';
	done_testing();
	exit;
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_parallel_maintenance_workers = 2
min_parallel_index_scan_size = 0
min_parallel_table_scan_size = 0
));
$node->start;

# Run $sql and return the progress trace it left in the server log, after
# checking it against the rules that hold for every command.
sub traced
{
	my ($name, $code) = @_;
	my $offset = -s $node->logfile;
	$code->();
	my $trace = ProgressCheck::parse_log(slurp_file($node->logfile, $offset));
	my @violations = ProgressCheck::check_trace($spec, $trace);
	is(scalar(@violations), 0, "$name: trace follows the rules")
	  or diag(
		join("\n",
			map { "$_->{rule}: $_->{detail}\n  $_->{text}" }
			  @violations[ 0 .. ($#violations < 9 ? $#violations : 9) ]));
	return $trace;
}

sub traced_sql
{
	my ($name, $sql) = @_;
	return traced($name, sub { $node->safe_psql('postgres', $sql) });
}

# Parameter number of a macro of progress.h.
sub p
{
	my ($macro) = @_;
	my $n = $spec->{macros}{$macro};
	die "unknown macro $macro" unless defined $n;
	return $n;
}

my $trace = traced_sql('probe', 'CREATE TABLE probe (a int); VACUUM probe');
my %runs = ProgressCheck::commands_run($trace);
is($runs{VACUUM}, 1, 'the server log has the trace of a VACUUM');

my $tempdir = PostgreSQL::Test::Utils::tempdir;
my $nrows = 3000;

$node->safe_psql(
	'postgres', qq(
CREATE TABLE prog (a int, b text);
INSERT INTO prog SELECT g, repeat('x', 100) FROM generate_series(1, $nrows) g;
COPY prog TO '$tempdir/prog.data';
TRUNCATE prog;
));
my $relid = $node->safe_psql('postgres', "SELECT 'prog'::regclass::oid");
my $file_size = -s "$tempdir/prog.data";

# COPY FROM a file: every row and every byte of the file is counted.
$trace = traced_sql('COPY FROM', "COPY prog FROM '$tempdir/prog.data'");
my ($copy) = ProgressCheck::final_values($trace, 'COPY', $relid);
is($copy->{ p('PROGRESS_COPY_TUPLES_PROCESSED') },
	$nrows, 'COPY FROM: tuples_processed');
is($copy->{ p('PROGRESS_COPY_BYTES_TOTAL') },
	$file_size, 'COPY FROM: bytes_total is the file size');
is($copy->{ p('PROGRESS_COPY_BYTES_PROCESSED') },
	$file_size, 'COPY FROM: bytes_processed reaches the file size');
is( $copy->{ p('PROGRESS_COPY_COMMAND') },
	p('PROGRESS_COPY_COMMAND_FROM'),
	'COPY FROM: command');
is( $copy->{ p('PROGRESS_COPY_TYPE') },
	p('PROGRESS_COPY_TYPE_FILE'),
	'COPY FROM: type');

# COPY TO a file.
$trace = traced_sql('COPY TO', "COPY prog TO '$tempdir/prog.out'");
($copy) = ProgressCheck::final_values($trace, 'COPY', $relid);
is($copy->{ p('PROGRESS_COPY_TUPLES_PROCESSED') },
	$nrows, 'COPY TO: tuples_processed');
is( $copy->{ p('PROGRESS_COPY_BYTES_PROCESSED') },
	-s "$tempdir/prog.out",
	'COPY TO: bytes_processed is the size of what was written');
is( $copy->{ p('PROGRESS_COPY_COMMAND') },
	p('PROGRESS_COPY_COMMAND_TO'),
	'COPY TO: command');

# CREATE INDEX goes through a single phase, building.
$trace = traced_sql('CREATE INDEX', 'CREATE INDEX prog_a ON prog (a)');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'CREATE_INDEX', $relid) ],
	[ [ p('PROGRESS_CREATEIDX_PHASE_BUILD') ] ],
	'CREATE INDEX: phases');
my ($idx) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is($idx->{ p('PROGRESS_CREATEIDX_TUPLES_DONE') },
	$nrows, 'CREATE INDEX: tuples_done');

# A parallel GIN build: the leader merges what the workers sorted, and
# counts each merged tuple once.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_gin (a int[]);
INSERT INTO prog_gin SELECT ARRAY[g % 100, g % 7] FROM generate_series(1, 3000) g;
));
my $gin_relid =
  $node->safe_psql('postgres', "SELECT 'prog_gin'::regclass::oid");
$trace = traced_sql('parallel GIN build',
	'CREATE INDEX prog_gin_a ON prog_gin USING gin (a)');
my ($gin) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $gin_relid);
cmp_ok($gin->{ p('PROGRESS_CREATEIDX_TUPLES_TOTAL') },
	'>', 0, 'parallel GIN build: tuples_total was reported');
is( $gin->{ p('PROGRESS_CREATEIDX_TUPLES_DONE') },
	$gin->{ p('PROGRESS_CREATEIDX_TUPLES_TOTAL') },
	'parallel GIN build: tuples_done ends at tuples_total');

# CREATE INDEX CONCURRENTLY goes through every phase, in order.
$trace = traced_sql('CREATE INDEX CONCURRENTLY',
	'CREATE INDEX CONCURRENTLY prog_b ON prog (b)');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'CREATE_INDEX', $relid) ],
	[
		[
			map { p("PROGRESS_CREATEIDX_PHASE_$_") }
			  qw(WAIT_1 BUILD WAIT_2 VALIDATE_IDXSCAN VALIDATE_SORT
			  VALIDATE_TABLESCAN WAIT_3)
		]
	],
	'CREATE INDEX CONCURRENTLY: phases');

# ANALYZE samples every block of a small table.
$trace = traced_sql('ANALYZE', 'ANALYZE prog');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'ANALYZE', $relid) ],
	[
		[
			map { p("PROGRESS_ANALYZE_PHASE_$_") }
			  qw(ACQUIRE_SAMPLE_ROWS COMPUTE_STATS FINALIZE_ANALYZE)
		]
	],
	'ANALYZE: phases');
my ($an) = ProgressCheck::final_values($trace, 'ANALYZE', $relid);
is( $an->{ p('PROGRESS_ANALYZE_BLOCKS_DONE') },
	$an->{ p('PROGRESS_ANALYZE_BLOCKS_TOTAL') },
	'ANALYZE: every block sampled');

# VACUUM with dead tuples and two indexes, then with an empty tail, which
# adds the truncate phase.
$node->safe_psql('postgres', 'DELETE FROM prog WHERE a % 3 = 0');
$trace = traced_sql('VACUUM', 'VACUUM prog');
my @vacuum = ProgressCheck::phases_of($trace, $spec, 'VACUUM', $relid);
is_deeply(
	\@vacuum,
	[
		[
			map { p("PROGRESS_VACUUM_PHASE_$_") }
			  qw(SCAN_HEAP VACUUM_INDEX VACUUM_HEAP INDEX_CLEANUP FINAL_CLEANUP)
		]
	],
	'VACUUM: phases');
my ($vac) = ProgressCheck::final_values($trace, 'VACUUM', $relid);
is( $vac->{ p('PROGRESS_VACUUM_HEAP_BLKS_SCANNED') },
	$vac->{ p('PROGRESS_VACUUM_TOTAL_HEAP_BLKS') },
	'VACUUM: every block scanned');
is($vac->{ p('PROGRESS_VACUUM_NUM_INDEX_VACUUMS') },
	1, 'VACUUM: one round of index vacuuming');

$node->safe_psql('postgres', "DELETE FROM prog WHERE a > $nrows / 2");
$trace = traced_sql('VACUUM with truncation', 'VACUUM prog');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'VACUUM', $relid) ],
	[
		[
			map { p("PROGRESS_VACUUM_PHASE_$_") }
			  qw(SCAN_HEAP VACUUM_INDEX VACUUM_HEAP INDEX_CLEANUP TRUNCATE
			  FINAL_CLEANUP)
		]
	],
	'VACUUM with truncation: phases');

# VACUUM with too little memory for all the dead items goes through several
# index vacuum cycles.  The dead item counters are documented as what was
# collected since the last cycle, so each new heap scan starts from 0.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_cycles (a int PRIMARY KEY);
INSERT INTO prog_cycles SELECT g FROM generate_series(1, 100000) g;
DELETE FROM prog_cycles WHERE a % 2 = 0;
));
my $cycles_relid =
  $node->safe_psql('postgres', "SELECT 'prog_cycles'::regclass::oid");
$trace = traced_sql('VACUUM in several cycles',
	"SET maintenance_work_mem = '64kB'; VACUUM prog_cycles");
my ($cyc) = ProgressCheck::final_values($trace, 'VACUUM', $cycles_relid);
cmp_ok($cyc->{ p('PROGRESS_VACUUM_NUM_INDEX_VACUUMS') },
	'>', 1, 'VACUUM in several cycles: more than one index vacuum cycle');
my @stale;
foreach my $events (values %$trace)
{
	my %v;
	foreach my $ev (grep { $_->{command} eq 'VACUUM' } @$events)
	{
		foreach my $ch (@{ $ev->{changes} || [] })
		{
			my ($n, $old, $new) = @$ch;
			push @stale,
			  "$ev->{text}: $v{ p('PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS') } dead item ids left"
			  if $n == p('PROGRESS_VACUUM_PHASE')
			  && $new == p('PROGRESS_VACUUM_PHASE_SCAN_HEAP')
			  && $old == p('PROGRESS_VACUUM_PHASE_VACUUM_HEAP')
			  && $v{ p('PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS') };
			$v{$n} = $new;
		}
	}
}
is_deeply(\@stale, [],
	'VACUUM in several cycles: each heap scan after the first starts with no dead items'
);

# Parallel index vacuuming: the workers' progress reaches the leader, so
# every index is counted before each index phase ends.
$node->safe_psql(
	'postgres', qq(
CREATE INDEX prog_ab ON prog (a, b);
DELETE FROM prog WHERE a % 5 = 0;
));
$trace = traced_sql('parallel VACUUM', 'VACUUM (PARALLEL 2) prog');

# A round of index processing ends when the phase changes or when the
# counters go back to 0; at that point every index must have been counted.
my (@rounds, %v);
my ($total, $processed, $phase) = (
	p('PROGRESS_VACUUM_INDEXES_TOTAL'),
	p('PROGRESS_VACUUM_INDEXES_PROCESSED'),
	p('PROGRESS_VACUUM_PHASE'));
foreach my $events (values %$trace)
{
	foreach my $ev (grep { $_->{command} eq 'VACUUM' } @$events)
	{
		%v = () if $ev->{event} eq 'start';
		my %changed = map { $_->[0] => $_->[2] } @{ $ev->{changes} || [] };
		if (($v{$total} // 0) > 0
			&& (   defined $changed{$phase}
				|| (defined $changed{$total} && $changed{$total} == 0)
				|| $ev->{event} eq 'end'))
		{
			push @rounds, [ $v{$processed} // 0, $v{$total} ];
		}
		$v{$_} = $changed{$_} foreach keys %changed;
	}
}
ok(@rounds > 0, 'parallel VACUUM: index rounds were reported');
is_deeply([ grep { $_->[0] != $_->[1] } @rounds ],
	[], 'parallel VACUUM: every round counts every index');

# REPACK rebuilds each index once, and says so: index_rebuild_count goes
# through 1, 2, ... n, one step per index.  Checking only the final value
# would miss a count that jumps ahead and comes back.  REPACK USING INDEX
# either sorts the heap or reads it through the index, depending on their
# costs; disabling index scans forces the sort.
my $nindexes = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_index WHERE indrelid = $relid");

sub rebuild_counts
{
	my ($trace, $relid) = @_;
	return [
		ProgressCheck::values_of(
			$trace, 'REPACK',
			$relid, p('PROGRESS_REPACK_INDEX_REBUILD_COUNT'))
	];
}
my @repack_sort = map { p("PROGRESS_REPACK_PHASE_$_") }
  qw(SEQ_SCAN_HEAP SORT_TUPLES WRITE_NEW_HEAP SWAP_REL_FILES REBUILD_INDEX
  FINAL_CLEANUP);
my @repack_index = map { p("PROGRESS_REPACK_PHASE_$_") }
  qw(INDEX_SCAN_HEAP SWAP_REL_FILES REBUILD_INDEX FINAL_CLEANUP);

$trace = traced_sql('REPACK USING INDEX with a sort',
	'SET enable_indexscan = off; REPACK prog USING INDEX prog_a');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'REPACK', $relid) ],
	[ \@repack_sort ],
	'REPACK USING INDEX with a sort: phases');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'REPACK USING INDEX with a sort: index_rebuild_count');

$trace = traced_sql('REPACK USING INDEX', 'REPACK prog USING INDEX prog_a');
my ($phases) = ProgressCheck::phases_of($trace, $spec, 'REPACK', $relid);
ok( "@$phases" eq "@repack_sort" || "@$phases" eq "@repack_index",
	'REPACK USING INDEX: phases of either way of ordering the heap'
) or diag("got phases @$phases");
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'REPACK USING INDEX: index_rebuild_count');

$trace = traced_sql('REPACK', 'REPACK prog');
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'REPACK', $relid) ],
	[
		[
			map { p("PROGRESS_REPACK_PHASE_$_") }
			  qw(SEQ_SCAN_HEAP SWAP_REL_FILES REBUILD_INDEX FINAL_CLEANUP)
		]
	],
	'REPACK: phases');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'REPACK: index_rebuild_count');
my ($rp) = ProgressCheck::final_values($trace, 'REPACK', $relid);
is( $rp->{ p('PROGRESS_REPACK_HEAP_BLKS_SCANNED') },
	$rp->{ p('PROGRESS_REPACK_TOTAL_HEAP_BLKS') },
	'REPACK: every block scanned');

# REPACK (CONCURRENTLY) rebuilds the indexes on the new heap itself, and
# counts them there.
$node->safe_psql(
	'postgres', qq(
CREATE TABLE prog_conc (a int PRIMARY KEY, b text);
INSERT INTO prog_conc SELECT g, repeat('x', 100) FROM generate_series(1, $nrows) g;
CREATE INDEX prog_conc_b ON prog_conc (b);
));
my $conc_relid =
  $node->safe_psql('postgres', "SELECT 'prog_conc'::regclass::oid");
$trace =
  traced_sql('REPACK (CONCURRENTLY)', 'REPACK (CONCURRENTLY) prog_conc');
is_deeply(
	rebuild_counts($trace, $conc_relid),
	[ [ 1, 2 ] ],
	'REPACK (CONCURRENTLY): index_rebuild_count');
($phases) = ProgressCheck::phases_of($trace, $spec, 'REPACK', $conc_relid);
ok( (grep { $_ == p('PROGRESS_REPACK_PHASE_CATCH_UP') } @$phases),
	'REPACK (CONCURRENTLY): goes through the catch-up phase'
) or diag("got phases @$phases");

# Base backups, reported by the walsender.  WAL is only transferred at the
# end when it is not streamed.
my @backup_phases = map { p("PROGRESS_BASEBACKUP_PHASE_$_") }
  qw(WAIT_CHECKPOINT ESTIMATE_BACKUP_SIZE STREAM_BACKUP WAIT_WAL_ARCHIVE);
$trace = traced(
	'BASEBACKUP',
	sub { $node->backup('backup', backup_options => ['--wal-method=stream']) }
);
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'BASEBACKUP', undef) ],
	[ \@backup_phases ],
	'BASEBACKUP: phases');
$trace = traced(
	'BASEBACKUP fetching WAL',
	sub { $node->backup('backup2', backup_options => ['--wal-method=fetch']) }
);
is_deeply(
	[ ProgressCheck::phases_of($trace, $spec, 'BASEBACKUP', undef) ],
	[ [ @backup_phases, p('PROGRESS_BASEBACKUP_PHASE_TRANSFER_WAL') ] ],
	'BASEBACKUP fetching WAL: phases');

# REINDEX reports each index as its own CREATE INDEX command, with the
# command set to REINDEX; REINDEX CONCURRENTLY goes through the phases of
# CREATE INDEX CONCURRENTLY.
$trace = traced_sql('REINDEX TABLE', 'REINDEX TABLE prog');
my @reindex = ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is(scalar(@reindex), $nindexes, 'REINDEX TABLE: one run per index');
is_deeply(
	[ map { $_->{ p('PROGRESS_CREATEIDX_COMMAND') } } @reindex ],
	[ (p('PROGRESS_CREATEIDX_COMMAND_REINDEX')) x $nindexes ],
	'REINDEX TABLE: command');

$trace = traced_sql('REINDEX INDEX CONCURRENTLY',
	'REINDEX INDEX CONCURRENTLY prog_a');
my ($reindex_conc) =
  ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is( $reindex_conc->{ p('PROGRESS_CREATEIDX_COMMAND') },
	p('PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY'),
	'REINDEX INDEX CONCURRENTLY: command');

# An index on a partitioned table counts the partitions it is built on.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_part (a int, b int) PARTITION BY RANGE (a);
CREATE TABLE prog_part_1 PARTITION OF prog_part FOR VALUES FROM (0) TO (1000);
CREATE TABLE prog_part_2 PARTITION OF prog_part FOR VALUES FROM (1000) TO (2000);
CREATE TABLE prog_part_3 PARTITION OF prog_part FOR VALUES FROM (2000) TO (3000);
INSERT INTO prog_part SELECT g, g % 10 FROM generate_series(0, 2999) g;
CREATE STATISTICS prog_part_stats ON a, b FROM prog_part;
));
my $part_relid =
  $node->safe_psql('postgres', "SELECT 'prog_part'::regclass::oid");
$trace = traced_sql('CREATE INDEX on a partitioned table',
	'CREATE INDEX prog_part_a ON prog_part (a)');
my ($part) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $part_relid);
is($part->{ p('PROGRESS_CREATEIDX_PARTITIONS_TOTAL') },
	3, 'CREATE INDEX on a partitioned table: partitions_total');
is($part->{ p('PROGRESS_CREATEIDX_PARTITIONS_DONE') },
	3, 'CREATE INDEX on a partitioned table: partitions_done');

# ANALYZE of a partitioned table samples the whole tree, then computes the
# extended statistics defined on it.
$trace = traced_sql('ANALYZE of a partitioned table', 'ANALYZE prog_part');
my @part_phases =
  ProgressCheck::phases_of($trace, $spec, 'ANALYZE', $part_relid);
ok( (grep { $_ == p('PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH') }
		  map { @$_ } @part_phases),
	'ANALYZE of a partitioned table: samples the inheritance tree'
) or diag(explain \@part_phases);
ok( (grep { $_ == p('PROGRESS_ANALYZE_PHASE_COMPUTE_EXT_STATS') }
		  map { @$_ } @part_phases),
	'ANALYZE of a partitioned table: computes the extended statistics'
) or diag(explain \@part_phases);
foreach my $an (ProgressCheck::final_values($trace, 'ANALYZE', $part_relid))
{
	is( $an->{ p('PROGRESS_ANALYZE_CHILD_TABLES_DONE') },
		$an->{ p('PROGRESS_ANALYZE_CHILD_TABLES_TOTAL') },
		'ANALYZE of a partitioned table: every child table done');
	is( $an->{ p('PROGRESS_ANALYZE_EXT_STATS_COMPUTED') },
		$an->{ p('PROGRESS_ANALYZE_EXT_STATS_TOTAL') },
		'ANALYZE of a partitioned table: every extended statistic computed'
	);
}

# COPY FROM counts the rows that WHERE excludes and the rows that
# ON_ERROR ignore skips, apart from the rows it loads.
$node->safe_psql('postgres', 'CREATE TABLE prog_copy (a int)');
my $copy_relid =
  $node->safe_psql('postgres', "SELECT 'prog_copy'::regclass::oid");
PostgreSQL::Test::Utils::append_to_file("$tempdir/copy.data",
	join('', map { $_ % 10 == 0 ? "bad\n" : "$_\n" } 1 .. 100));
$trace = traced_sql('COPY FROM with WHERE and ON_ERROR',
	"COPY prog_copy FROM '$tempdir/copy.data' WITH (ON_ERROR ignore) WHERE a > 50"
);
($copy) = ProgressCheck::final_values($trace, 'COPY', $copy_relid);
is($copy->{ p('PROGRESS_COPY_TUPLES_SKIPPED') },
	10, 'COPY FROM with ON_ERROR ignore: tuples_skipped');
is($copy->{ p('PROGRESS_COPY_TUPLES_EXCLUDED') },
	45, 'COPY FROM with WHERE: tuples_excluded');
is($copy->{ p('PROGRESS_COPY_TUPLES_PROCESSED') },
	45, 'COPY FROM with WHERE and ON_ERROR: tuples_processed');

# VACUUM FULL is reported as REPACK, and rebuilds every index.
$trace = traced_sql('VACUUM FULL', 'VACUUM FULL prog');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'VACUUM FULL: index_rebuild_count');

# Online changes of data checksums: a launcher that goes over the
# databases, and for enabling, one worker per database that goes over its
# relations.  The counters start at -1, shown as NULL.
sub checksums_trace
{
	my ($name, $sql, $state) = @_;
	return traced(
		$name,
		sub {
			$node->safe_psql('postgres', $sql);
			$node->poll_query_until('postgres',
				"SELECT setting = '$state' FROM pg_settings WHERE name = 'data_checksums'"
			) or die "data_checksums did not become $state";
			$node->poll_query_until('postgres',
				"SELECT count(*) = 0 FROM pg_stat_activity WHERE backend_type = 'datachecksums launcher'"
			) or die 'the datachecksums launcher did not exit';
		});
}

# The final values of each run of DATACHECKSUMS by a process whose log
# lines name $type.
sub checksums_runs
{
	my ($trace, $type) = @_;
	my %by_type = map { $_ => $trace->{$_} }
	  grep { grep { $_->{text} =~ /\Q$type\E\[/ } @{ $trace->{$_} } }
	  keys %$trace;
	return ProgressCheck::final_values(\%by_type, 'DATACHECKSUMS', 0);
}

sub checksums_phases
{
	my ($trace, $type) = @_;
	my %by_type = map { $_ => $trace->{$_} }
	  grep { grep { $_->{text} =~ /\Q$type\E\[/ } @{ $trace->{$_} } }
	  keys %$trace;
	return ProgressCheck::phases_of(\%by_type, $spec, 'DATACHECKSUMS', 0);
}

$trace = checksums_trace('DATACHECKSUMS disable',
	'SELECT pg_disable_data_checksums()', 'off');
is_deeply(
	[ checksums_phases($trace, 'datachecksums launcher') ],
	[
		[
			map { p("PROGRESS_DATACHECKSUMS_PHASE_$_") }
			  qw(DISABLING DONE)
		]
	],
	'DATACHECKSUMS disable: phases of the launcher');

$trace = checksums_trace('DATACHECKSUMS enable',
	'SELECT pg_enable_data_checksums(0, 100)', 'on');
is_deeply(
	[ checksums_phases($trace, 'datachecksums launcher') ],
	[
		[
			map { p("PROGRESS_DATACHECKSUMS_PHASE_$_") }
			  qw(WAITING_BARRIER DONE)
		]
	],
	'DATACHECKSUMS enable: phases of the launcher');
my ($launcher) = checksums_runs($trace, 'datachecksums launcher');
cmp_ok($launcher->{ p('PROGRESS_DATACHECKSUMS_DBS_TOTAL') },
	'>', 0, 'DATACHECKSUMS enable: databases_total');
is( $launcher->{ p('PROGRESS_DATACHECKSUMS_DBS_DONE') },
	$launcher->{ p('PROGRESS_DATACHECKSUMS_DBS_TOTAL') },
	'DATACHECKSUMS enable: every database done');
my @workers = checksums_runs($trace, 'datachecksums worker');
is( scalar(@workers),
	$launcher->{ p('PROGRESS_DATACHECKSUMS_DBS_TOTAL') },
	'DATACHECKSUMS enable: one worker per database');
is_deeply(
	[
		grep {
			$_->{ p('PROGRESS_DATACHECKSUMS_RELS_DONE') } !=
			  $_->{ p('PROGRESS_DATACHECKSUMS_RELS_TOTAL') }
		} @workers
	],
	[],
	'DATACHECKSUMS enable: each worker does every relation');

$node->stop;

done_testing();
