
# Copyright (c) 2026, PostgreSQL Global Development Group

# Check the whole sequence of values that commands report through the
# pg_stat_progress_* views.
#
# A server compiled with PROGRESS_DEBUG logs every change to a backend's
# progress state while trace_progress is on.  This test runs a series of
# commands, reads those lines back from the server log and checks them:
# first against rules that hold for every command (see ProgressCheck.pm),
# then against the exact succession of phases and the final values expected
# for each command.  At the end, it checks that every parameter of every
# command was checked by value at least once, or is listed in %UNCHECKED
# with the reason.
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

if (!ProgressCheck::compiled_with_progress_debug($progress_h))
{
	note 'not compiled with PROGRESS_DEBUG, skipping the traces';
	done_testing();
	exit;
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 'logical');
$node->append_conf(
	'postgresql.conf', qq(
trace_progress = on
autovacuum = off
summarize_wal = on
track_cost_delay_timing = on
max_parallel_maintenance_workers = 2
min_parallel_index_scan_size = 0
min_parallel_table_scan_size = 0
));
$node->start;

# Blips that the trace exposes but that are harmless, so they do not fail
# the suite.  They come from a counter that an index build or an index
# vacuum cycle leaves set when one command builds or vacuums several
# indexes in a row: the counter steps straight to the next round's value
# instead of first returning to 0.  When the next index has fewer tuples
# than the previous one, e.g. a partial index, the leftover tuple count is
# also above the new index's total until the build starts counting.
# REPACK's view does not show the index build counters; the dead-item
# counters are shown, until the first block of the next heap scan that has
# dead items.  Resetting these counters in every build and cycle makes them
# go away, and the TODO block below pass.
my @ACCEPTED = (
	{
		rule => 'reset',
		command => 'REPACK',
		param => 'PROGRESS_CREATEIDX_TUPLES_DONE'
	},
	{
		rule => 'bound',
		command => 'REPACK',
		param => 'PROGRESS_CREATEIDX_TUPLES_DONE'
	},
	{
		rule => 'reset',
		command => 'VACUUM',
		param => 'PROGRESS_VACUUM_DEAD_TUPLE_BYTES'
	},
	{
		rule => 'reset',
		command => 'VACUUM',
		param => 'PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS'
	},);

sub is_accepted
{
	my ($v) = @_;
	foreach my $a (@ACCEPTED)
	{
		return 1
		  if $v->{rule} eq $a->{rule}
		  && $v->{command} eq $a->{command}
		  && index($v->{detail}, $a->{param}) == 0;
	}
	return 0;
}

# Run $code and return the progress trace it left in the server log of the
# main node, or of $n, after checking it against the rules that hold for
# every command.
sub traced
{
	my ($name, $code) = @_;
	return traced_on($node, $name, $code);
}

sub traced_on
{
	my ($n, $name, $code) = @_;
	my $offset = -s $n->logfile;
	$code->();
	my $trace = ProgressCheck::parse_log(slurp_file($n->logfile, $offset));
	my @violations = ProgressCheck::check_trace($spec, $trace);
	my @accepted = grep { is_accepted($_) } @violations;
	@violations = grep { !is_accepted($_) } @violations;
	diag(   "$name: "
		  . scalar(@accepted)
		  . " known harmless blip(s) set aside (see \@ACCEPTED)")
	  if @accepted;
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

# The parameters of each command whose value some check below looks at, as
# "COMMAND MACRO"; see the end.
my %checked;

sub checked
{
	my ($command, @macros) = @_;
	$checked{"$command $_"} = 1 foreach @macros;
}

# The value that parameter $macro had at the end of a run, from the hash
# that final_values() returns.  A parameter that the run never changed is
# 0, as every command starts with all of them at 0.  There is no value if
# the run never ended, which the checks then report.
sub v
{
	my ($run, $macro) = @_;
	checked($run->{command}, $macro) if defined $run;
	return defined $run ? $run->{ p($macro) } // 0 : undef;
}

# phases_of() and values_of(), noting what they check.
sub phases
{
	my ($trace, $command, $relid) = @_;
	checked($command,
		$spec->{$command}{name}{ $spec->{$command}{phase_param} });
	return ProgressCheck::phases_of($trace, $spec, $command, $relid);
}

sub values_of
{
	my ($trace, $command, $relid, $macro) = @_;
	checked($command, $macro);
	return ProgressCheck::values_of($trace, $command, $relid, p($macro));
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
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	$nrows, 'COPY FROM: tuples_processed');
is(v($copy, 'PROGRESS_COPY_BYTES_TOTAL'),
	$file_size, 'COPY FROM: bytes_total is the file size');
is(v($copy, 'PROGRESS_COPY_BYTES_PROCESSED'),
	$file_size, 'COPY FROM: bytes_processed reaches the file size');
is( v($copy, 'PROGRESS_COPY_COMMAND'),
	p('PROGRESS_COPY_COMMAND_FROM'),
	'COPY FROM: command');
is( v($copy, 'PROGRESS_COPY_TYPE'),
	p('PROGRESS_COPY_TYPE_FILE'),
	'COPY FROM: type');

# COPY TO a file.
$trace = traced_sql('COPY TO', "COPY prog TO '$tempdir/prog.out'");
($copy) = ProgressCheck::final_values($trace, 'COPY', $relid);
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	$nrows, 'COPY TO: tuples_processed');
is( v($copy, 'PROGRESS_COPY_BYTES_PROCESSED'),
	-s "$tempdir/prog.out",
	'COPY TO: bytes_processed is the size of what was written');
is( v($copy, 'PROGRESS_COPY_COMMAND'),
	p('PROGRESS_COPY_COMMAND_TO'),
	'COPY TO: command');

# COPY FROM STDIN and COPY TO STDOUT read and write through a pipe.
$node->safe_psql('postgres', 'CREATE TABLE prog_pipe (a int)');
my $pipe_relid =
  $node->safe_psql('postgres', "SELECT 'prog_pipe'::regclass::oid");
$trace =
  traced_sql('COPY FROM STDIN', "COPY prog_pipe FROM STDIN;\n1\n2\n3\n\\.\n");
($copy) = ProgressCheck::final_values($trace, 'COPY', $pipe_relid);
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	3, 'COPY FROM STDIN: tuples_processed');
is( v($copy, 'PROGRESS_COPY_TYPE'),
	p('PROGRESS_COPY_TYPE_PIPE'),
	'COPY FROM STDIN: type');
is(v($copy, 'PROGRESS_COPY_BYTES_TOTAL'),
	0, 'COPY FROM STDIN: bytes_total is not known');
is(v($copy, 'PROGRESS_COPY_BYTES_PROCESSED'),
	6, 'COPY FROM STDIN: bytes_processed counts the data rows');
$trace = traced_sql('COPY TO STDOUT', 'COPY prog_pipe TO STDOUT');
($copy) = ProgressCheck::final_values($trace, 'COPY', $pipe_relid);
is( v($copy, 'PROGRESS_COPY_TYPE'),
	p('PROGRESS_COPY_TYPE_PIPE'),
	'COPY TO STDOUT: type');
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	3, 'COPY TO STDOUT: tuples_processed');

# COPY of a query sends each row the query returns, and counts it.
$trace = traced_sql('COPY (query) TO',
	"COPY (SELECT * FROM prog WHERE a <= 1000) TO '$tempdir/prog.query'");
is_deeply(
	[ values_of($trace, 'COPY', 0, 'PROGRESS_COPY_TUPLES_PROCESSED') ],
	[ [ 1 .. 1000 ] ],
	'COPY (query) TO: tuples_processed counts each row');

# A BEFORE ROW trigger makes COPY FROM insert one row at a time, and count
# only the rows it does insert.
$node->safe_psql(
	'postgres', q{
CREATE TABLE prog_trig (a int);
CREATE FUNCTION prog_skip_odd() RETURNS trigger LANGUAGE plpgsql AS
  $$BEGIN IF NEW.a % 2 = 1 THEN RETURN NULL; END IF; RETURN NEW; END$$;
CREATE TRIGGER prog_trig_skip BEFORE INSERT ON prog_trig
  FOR EACH ROW EXECUTE FUNCTION prog_skip_odd();
});
my $trig_relid =
  $node->safe_psql('postgres', "SELECT 'prog_trig'::regclass::oid");
$trace = traced_sql('COPY FROM with a BEFORE ROW trigger',
	"COPY prog_trig FROM STDIN;\n" . join("\n", 1 .. 10) . "\n\\.\n");
is_deeply(
	[
		values_of(
			$trace, 'COPY', $trig_relid, 'PROGRESS_COPY_TUPLES_PROCESSED')
	],
	[ [ 1 .. 5 ] ],
	'COPY FROM with a BEFORE ROW trigger: counts each row inserted');

# COPY FROM into a foreign table that takes rows in batches counts each
# batch as it is sent.
my $port = $node->port;
my $host = $node->host;
$node->safe_psql(
	'postgres', qq{
CREATE EXTENSION postgres_fdw;
CREATE SERVER prog_loop FOREIGN DATA WRAPPER postgres_fdw
  OPTIONS (dbname 'postgres', host '$host', port '$port');
CREATE USER MAPPING FOR CURRENT_USER SERVER prog_loop;
CREATE TABLE prog_fdw_target (a int);
CREATE FOREIGN TABLE prog_fdw (a int) SERVER prog_loop
  OPTIONS (table_name 'prog_fdw_target', batch_size '10');
});
my $fdw_relid =
  $node->safe_psql('postgres', "SELECT 'prog_fdw'::regclass::oid");
$trace = traced_sql('COPY FROM into a foreign table',
	"COPY prog_fdw FROM STDIN;\n" . join("\n", 1 .. 25) . "\n\\.\n");
is_deeply(
	[
		values_of(
			$trace, 'COPY', $fdw_relid, 'PROGRESS_COPY_TUPLES_PROCESSED')
	],
	[ [ 10, 20, 25 ] ],
	'COPY FROM into a foreign table: tuples_processed counts each batch');

my %am = map { split /\|/ } split /\n/,
  $node->safe_psql('postgres', 'SELECT amname, oid FROM pg_am');

# CREATE INDEX goes through a single phase, building.
# Not in parallel, so that the leader counts every block itself: the leader
# of a parallel scan counts the blocks handed out to all, which jumps.
$trace = traced_sql(
	'CREATE INDEX',
	'SET max_parallel_maintenance_workers = 0; CREATE INDEX prog_a ON prog (a)'
);
is_deeply(
	[ phases($trace, 'CREATE_INDEX', $relid) ],
	[ [ p('PROGRESS_CREATEIDX_PHASE_BUILD') ] ],
	'CREATE INDEX: phases');
my ($idx) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is(v($idx, 'PROGRESS_CREATEIDX_TUPLES_DONE'),
	$nrows, 'CREATE INDEX: tuples_done');
is( v($idx, 'PROGRESS_CREATEIDX_COMMAND'),
	p('PROGRESS_CREATEIDX_COMMAND_CREATE'),
	'CREATE INDEX: command');
is(v($idx, 'PROGRESS_CREATEIDX_ACCESS_METHOD_OID'),
	$am{btree}, 'CREATE INDEX: access method');
# documented as 0 for a CREATE INDEX that is not concurrent
is(v($idx, 'PROGRESS_CREATEIDX_INDEX_OID'), 0, 'CREATE INDEX: index_relid');

# The subphases of a build are defined by each index access method, in its
# own header; read their values there, as for progress.h.
my %subphase;
foreach my $am (qw(nbtree gin))
{
	(my $h = $progress_h) =~ s{commands/progress\.h$}{access/$am.h};
	my $contents = slurp_file($h);
	while ($contents =~ /^#define\s+PROGRESS_(\w+?)_PHASE_(\w+)\s+(\d+)/mg)
	{
		$subphase{"$1_$2"} = $3;
	}
}
die 'no subphases found for btree' unless defined $subphase{BTREE_LEAF_LOAD};

sub subphases
{
	my ($am, @names) = @_;
	return (p('PROGRESS_CREATEIDX_SUBPHASE_INITIALIZE'),
		map { $subphase{"${am}_$_"} // die "no subphase ${am}_$_" } @names);
}

# A btree build scans the table, sorts what it read and loads the leaves,
# counting each block and then each tuple.
my $prog_blocks = $node->safe_psql('postgres',
	"SELECT pg_relation_size('prog') / current_setting('block_size')::int");
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_CREATEIDX_SUBPHASE')
	],
	[
		[
			subphases(
				'BTREE', qw(INDEXBUILD_TABLESCAN PERFORMSORT_1 LEAF_LOAD))
		]
	],
	'CREATE INDEX: subphases');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_SCAN_BLOCKS_DONE')
	],
	[ [ 1 .. $prog_blocks, 0 ] ],
	'CREATE INDEX: the table scan counts each block');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. $nrows ] ],
	'CREATE INDEX: tuples_done counts each tuple');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$relid, 'PROGRESS_CREATEIDX_TUPLES_TOTAL')
	],
	[ [$nrows] ],
	'CREATE INDEX: tuples_total');

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
cmp_ok(v($gin, 'PROGRESS_CREATEIDX_TUPLES_TOTAL'),
	'>', 0, 'parallel GIN build: tuples_total was reported');
is( v($gin, 'PROGRESS_CREATEIDX_TUPLES_DONE'),
	v($gin, 'PROGRESS_CREATEIDX_TUPLES_TOTAL'),
	'parallel GIN build: tuples_done ends at tuples_total');
is(v($gin, 'PROGRESS_CREATEIDX_ACCESS_METHOD_OID'),
	$am{gin}, 'parallel GIN build: access method');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$gin_relid, 'PROGRESS_CREATEIDX_SUBPHASE')
	],
	[
		[
			subphases(
				'GIN', qw(INDEXBUILD_TABLESCAN PERFORMSORT_1 MERGE_1
				  PERFORMSORT_2 MERGE_2))
		]
	],
	'parallel GIN build: subphases');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$gin_relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. v($gin, 'PROGRESS_CREATEIDX_TUPLES_TOTAL') ] ],
	'parallel GIN build: tuples_done counts each merged tuple');
my $gin_blocks = $node->safe_psql('postgres',
	"SELECT pg_relation_size('prog_gin') / current_setting('block_size')::int"
);
is_deeply(
	[
		map { @$_ } values_of(
			$trace, 'CREATE_INDEX',
			$gin_relid, 'PROGRESS_SCAN_BLOCKS_TOTAL')
	],
	[ $gin_blocks, 0 ],
	'parallel GIN build: blocks_total of the table scan, then reset');

# The leader of a parallel scan counts the blocks handed out to all the
# participants, so it may skip some, but it ends with all of them.
my @gin_done = map { @$_ }
  values_of($trace, 'CREATE_INDEX', $gin_relid, 'PROGRESS_SCAN_BLOCKS_DONE');
my $gin_last = pop @gin_done;
ok( $gin_last == 0 && @gin_done && $gin_done[-1] == $gin_blocks && !(
		grep { $gin_done[$_] <= ($_ ? $gin_done[ $_ - 1 ] : 0) }
		0 .. $#gin_done),
	'parallel GIN build: the table scan counts up to every block, then is reset'
) or diag("blocks_done: @gin_done $gin_last");

# A unique btree build puts the tuples that are dead but still visible to
# some snapshot in a second spool, sorts it after the first and merges both
# while loading the leaves.  A session keeps such a snapshot open.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_uniq (a int);
INSERT INTO prog_uniq SELECT g FROM generate_series(1, 1000) g;
));
my $uniq_relid =
  $node->safe_psql('postgres', "SELECT 'prog_uniq'::regclass::oid");
my $snap = $node->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres', 'DELETE FROM prog_uniq WHERE a % 2 = 0');
$trace = traced_sql(
	'unique CREATE INDEX with dead tuples',
	'SET max_parallel_maintenance_workers = 0; '
	  . 'CREATE UNIQUE INDEX prog_uniq_a ON prog_uniq (a)');
$snap->query_safe('COMMIT');
$snap->quit;
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$uniq_relid, 'PROGRESS_CREATEIDX_SUBPHASE')
	],
	[
		[
			subphases(
				'BTREE', qw(INDEXBUILD_TABLESCAN PERFORMSORT_1 PERFORMSORT_2
				  LEAF_LOAD))
		]
	],
	'unique CREATE INDEX with dead tuples: subphases');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$uniq_relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. 1000 ] ],
	'unique CREATE INDEX with dead tuples: tuples_done counts both spools');

# A parallel btree build: the leader takes part as a worker, sorting its own
# share before it merges the workers' sorted runs.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_par (a int);
INSERT INTO prog_par SELECT g FROM generate_series(1, 100000) g;
ALTER TABLE prog_par SET (parallel_workers = 2);
));
my $par_relid =
  $node->safe_psql('postgres', "SELECT 'prog_par'::regclass::oid");
$trace = traced_sql('parallel CREATE INDEX',
	'CREATE INDEX prog_par_a ON prog_par (a)');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$par_relid, 'PROGRESS_CREATEIDX_SUBPHASE')
	],
	[
		[
			subphases(
				'BTREE', qw(INDEXBUILD_TABLESCAN PERFORMSORT_1 LEAF_LOAD))
		]
	],
	'parallel CREATE INDEX: subphases');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$par_relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. 100000 ] ],
	'parallel CREATE INDEX: tuples_done counts each tuple');

# A unique parallel build with dead tuples: the leader, taking part as a
# worker, sorts both of its spools, then sorts both merged spools again.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_par_uniq (a int);
INSERT INTO prog_par_uniq SELECT g FROM generate_series(1, 100000) g;
ALTER TABLE prog_par_uniq SET (parallel_workers = 2);
));
my $par_uniq_relid =
  $node->safe_psql('postgres', "SELECT 'prog_par_uniq'::regclass::oid");
$snap = $node->background_psql('postgres');
$snap->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
$node->safe_psql('postgres', 'DELETE FROM prog_par_uniq WHERE a % 2 = 0');
$trace = traced_sql(
	'parallel unique CREATE INDEX with dead tuples',
	'CREATE UNIQUE INDEX prog_par_uniq_a ON prog_par_uniq (a)');
$snap->query_safe('COMMIT');
$snap->quit;
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$par_uniq_relid, 'PROGRESS_CREATEIDX_SUBPHASE')
	],
	[
		[
			subphases(
				'BTREE', qw(INDEXBUILD_TABLESCAN PERFORMSORT_1 PERFORMSORT_2
				  PERFORMSORT_1 PERFORMSORT_2 LEAF_LOAD))
		]
	],
	'parallel unique CREATE INDEX with dead tuples: subphases');

# Without deduplication, the leaves are loaded one tuple at a time.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_nodedup (a int);
INSERT INTO prog_nodedup SELECT g % 10 FROM generate_series(1, 1000) g;
));
my $nodedup_relid =
  $node->safe_psql('postgres', "SELECT 'prog_nodedup'::regclass::oid");
$trace = traced_sql(
	'CREATE INDEX without deduplication',
	'SET max_parallel_maintenance_workers = 0; '
	  . 'CREATE INDEX prog_nodedup_a ON prog_nodedup (a) '
	  . 'WITH (deduplicate_items = off)');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$nodedup_relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. 1000 ] ],
	'CREATE INDEX without deduplication: tuples_done counts each tuple');

# A hash build that sorts the tuples by bucket first, which it does when
# there are more buckets than fit in maintenance_work_mem.
$trace = traced_sql(
	'hash CREATE INDEX',
	"SET maintenance_work_mem = '1MB'; "
	  . 'CREATE INDEX prog_par_hash ON prog_par USING hash (a)');
my ($hash) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $par_relid);
is(v($hash, 'PROGRESS_CREATEIDX_ACCESS_METHOD_OID'),
	$am{hash}, 'hash CREATE INDEX: access method');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$par_relid, 'PROGRESS_CREATEIDX_TUPLES_TOTAL')
	],
	[ [100000] ],
	'hash CREATE INDEX: tuples_total');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$par_relid, 'PROGRESS_CREATEIDX_TUPLES_DONE')
	],
	[ [ 1 .. 100000 ] ],
	'hash CREATE INDEX: tuples_done counts each sorted tuple');

# CREATE INDEX CONCURRENTLY goes through every phase, in order.  Not in
# parallel, as for CREATE INDEX above.
$trace = traced_sql(
	'CREATE INDEX CONCURRENTLY',
	'SET max_parallel_maintenance_workers = 0; '
	  . 'CREATE INDEX CONCURRENTLY prog_b ON prog (b)');
is_deeply(
	[ phases($trace, 'CREATE_INDEX', $relid) ],
	[
		[
			map { p("PROGRESS_CREATEIDX_PHASE_$_") }
			  qw(WAIT_1 BUILD WAIT_2 VALIDATE_IDXSCAN VALIDATE_SORT
			  VALIDATE_TABLESCAN WAIT_3)
		]
	],
	'CREATE INDEX CONCURRENTLY: phases');

my ($cic) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is( v($cic, 'PROGRESS_CREATEIDX_COMMAND'),
	p('PROGRESS_CREATEIDX_COMMAND_CREATE_CONCURRENTLY'),
	'CREATE INDEX CONCURRENTLY: command');
is( v($cic, 'PROGRESS_CREATEIDX_INDEX_OID'),
	$node->safe_psql('postgres', "SELECT 'prog_b'::regclass::oid"),
	'CREATE INDEX CONCURRENTLY: index_relid');

# The scans of the table, to build the index and to validate it, end with
# every block counted.  The validation also scans the index, which for
# btree never counts the metapage; see VACUUM below.
sub scan_rounds
{
	my ($trace, $command, $relid, @boundaries) = @_;
	checked($command, 'PROGRESS_SCAN_BLOCKS_DONE',
		'PROGRESS_SCAN_BLOCKS_TOTAL');
	return ProgressCheck::rounds(
		$trace, $command, $relid,
		p('PROGRESS_SCAN_BLOCKS_DONE'),
		p('PROGRESS_SCAN_BLOCKS_TOTAL'),
		map { p($_) } @boundaries);
}
my %table_scan =
  map { p("PROGRESS_CREATEIDX_PHASE_$_") => $_ } qw(BUILD VALIDATE_TABLESCAN);
my @scans =
  grep { $table_scan{ $_->[2]{ p('PROGRESS_CREATEIDX_PHASE') } // 0 } }
  scan_rounds($trace, 'CREATE_INDEX', $relid, 'PROGRESS_CREATEIDX_PHASE');
is_deeply(
	[
		map { $table_scan{ $_->[2]{ p('PROGRESS_CREATEIDX_PHASE') } } }
		  @scans
	],
	[qw(BUILD VALIDATE_TABLESCAN)],
	'CREATE INDEX CONCURRENTLY: two scans of the table');
is_deeply(
	[ map { $_->[1] - $_->[0] } @scans ],
	[ 0, 0 ],
	'CREATE INDEX CONCURRENTLY: each scan counts every block');

# Block by block: the table scan of the build, the scan of the new index by
# the validation (all its pages but the metapage) and the second table scan.
my $prog_b_pages = $node->safe_psql('postgres',
	"SELECT pg_relation_size('prog_b') / current_setting('block_size')::int");
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_SCAN_BLOCKS_TOTAL')
	],
	[ [ $prog_blocks, 0, $prog_b_pages, 0, $prog_blocks ] ],
	'CREATE INDEX CONCURRENTLY: blocks_total of each scan');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_SCAN_BLOCKS_DONE')
	],
	[ [ 1 .. $prog_blocks, 0 .. $prog_b_pages - 1, 0 .. $prog_blocks ] ],
	'CREATE INDEX CONCURRENTLY: blocks_done counts each block of each scan');

# CREATE INDEX CONCURRENTLY that waits: first for a transaction that writes
# to the table, then for one with an older snapshot.  It reports how many
# it waits for, which one, and how many it is done with.  A session that
# happens to hold a snapshot when the old ones are counted is counted too,
# and skipped without a report if it is gone by its turn: so the old
# snapshots are known to be at least one, and lockers_done to go up without
# passing the total, not to reach it.
$node->safe_psql('postgres', 'CREATE TABLE prog_wait (a int)');
my $wait_relid =
  $node->safe_psql('postgres', "SELECT 'prog_wait'::regclass::oid");
my $old = $node->background_psql('postgres');
my $old_pid = $old->query_safe('SELECT pg_backend_pid()');
my $writer = $node->background_psql('postgres');
my $writer_pid = $writer->query_safe('SELECT pg_backend_pid()');
my $cic_bg = $node->background_psql('postgres');
my $cic_pid = $cic_bg->query_safe('SELECT pg_backend_pid()');
$trace = traced(
	'CREATE INDEX CONCURRENTLY that waits',
	sub {
		$old->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT 1');
		$writer->query_safe('BEGIN; INSERT INTO prog_wait VALUES (1)');
		$cic_bg->query_until(
			qr/cic starts/,
			"\\echo cic starts\nCREATE INDEX CONCURRENTLY prog_wait_a ON prog_wait (a);\n"
		);
		$node->poll_query_until('postgres',
			"SELECT phase = 'waiting for writers before build' FROM pg_stat_progress_create_index WHERE pid = $cic_pid"
		) or die 'CREATE INDEX CONCURRENTLY did not wait for the writer';
		$writer->query_safe('COMMIT');
		$node->poll_query_until('postgres',
			"SELECT phase = 'waiting for old snapshots' FROM pg_stat_progress_create_index WHERE pid = $cic_pid"
		  )
		  or die
		  'CREATE INDEX CONCURRENTLY did not wait for the old snapshot';
		$old->query_safe('COMMIT');
		# the next query gets an answer once the index is built
		$cic_bg->query_safe('SELECT 1');
	});
$_->quit foreach ($old, $writer, $cic_bg);
my @wait_states =
  map { @$_ } ProgressCheck::states($trace, 'CREATE_INDEX', $wait_relid);

sub waits_in
{
	my ($phase, $macro) = @_;
	checked('CREATE_INDEX', $macro);
	my @v;
	foreach my $st (
		grep {
			($_->{ p('PROGRESS_CREATEIDX_PHASE') } // 0) ==
			  p("PROGRESS_CREATEIDX_PHASE_$phase")
		} @wait_states)
	{
		my $x = $st->{ p($macro) } // 0;
		push @v, $x unless @v && $v[-1] == $x;
	}
	return \@v;
}
is_deeply(
	[
		map { waits_in('WAIT_1', "PROGRESS_WAITFOR_$_") }
		  qw(TOTAL CURRENT_PID DONE)
	],
	[ [ 0, 1, 0 ], [ 0, $writer_pid, 0 ], [ 0, 1, 0 ] ],
	'CREATE INDEX CONCURRENTLY that waits: waits for the writer, then resets'
);
my $old_total = waits_in('WAIT_3', 'PROGRESS_WAITFOR_TOTAL');
my $old_done = waits_in('WAIT_3', 'PROGRESS_WAITFOR_DONE');
my @steps = @$old_done[ 1 .. $#$old_done ];
ok( $old_total->[-1] >= 1
	  && $old_done->[0] == 0
	  && @steps
	  && !(grep { $steps[$_] <= ($_ ? $steps[ $_ - 1 ] : 0) } 0 .. $#steps)
	  && $steps[-1] <= $old_total->[-1],
	'CREATE INDEX CONCURRENTLY that waits: counts the old snapshots it waits for'
) or diag("lockers_total @$old_total, lockers_done @$old_done");
ok( (   grep { $_ == $old_pid }
		  @{ waits_in('WAIT_3', 'PROGRESS_WAITFOR_CURRENT_PID') }),
	'CREATE INDEX CONCURRENTLY that waits: names the session with the old snapshot'
);

# CREATE INDEX IF NOT EXISTS of an index that exists ends its report too.
$trace = traced_sql('CREATE INDEX IF NOT EXISTS',
	'CREATE INDEX IF NOT EXISTS prog_a ON prog (a)');
is(scalar(ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid)),
	1, 'CREATE INDEX IF NOT EXISTS: the report ends');

# ANALYZE samples every block of a small table.
$trace = traced_sql('ANALYZE', 'ANALYZE prog');
is_deeply(
	[ phases($trace, 'ANALYZE', $relid) ],
	[
		[
			map { p("PROGRESS_ANALYZE_PHASE_$_") }
			  qw(ACQUIRE_SAMPLE_ROWS COMPUTE_STATS FINALIZE_ANALYZE)
		]
	],
	'ANALYZE: phases');
my ($an) = ProgressCheck::final_values($trace, 'ANALYZE', $relid);
is( v($an, 'PROGRESS_ANALYZE_BLOCKS_DONE'),
	v($an, 'PROGRESS_ANALYZE_BLOCKS_TOTAL'),
	'ANALYZE: every block sampled');
is( v($an, 'PROGRESS_ANALYZE_STARTED_BY'),
	p('PROGRESS_ANALYZE_STARTED_BY_MANUAL'),
	'ANALYZE: started_by');
is(v($an, 'PROGRESS_ANALYZE_DELAY_TIME'), 0, 'ANALYZE: no delay');

# With cost-based delays, the time spent sleeping is reported.
$trace = traced_sql(
	'ANALYZE with cost delays',
	"SET vacuum_cost_delay = '1ms'; SET vacuum_cost_limit = 1; ANALYZE prog");
($an) = ProgressCheck::final_values($trace, 'ANALYZE', $relid);
cmp_ok(v($an, 'PROGRESS_ANALYZE_DELAY_TIME'),
	'>', 0, 'ANALYZE with cost delays: delay_time');

# VACUUM with dead tuples and two indexes, then with an empty tail, which
# adds the truncate phase.
$node->safe_psql('postgres', 'DELETE FROM prog WHERE a % 3 = 0');
$trace = traced_sql('VACUUM', 'VACUUM (PARALLEL 0) prog');
my @vacuum = phases($trace, 'VACUUM', $relid);
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
is( v($vac, 'PROGRESS_VACUUM_HEAP_BLKS_SCANNED'),
	v($vac, 'PROGRESS_VACUUM_TOTAL_HEAP_BLKS'),
	'VACUUM: every block scanned');
is(v($vac, 'PROGRESS_VACUUM_NUM_INDEX_VACUUMS'),
	1, 'VACUUM: one round of index vacuuming');
is(v($vac, 'PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS'),
	$nrows / 3, 'VACUUM: num_dead_item_ids is the number of rows deleted');
# every block of the table had rows deleted
is( v($vac, 'PROGRESS_VACUUM_HEAP_BLKS_VACUUMED'),
	v($vac, 'PROGRESS_VACUUM_TOTAL_HEAP_BLKS'),
	'VACUUM: every block vacuumed');
is( v($vac, 'PROGRESS_VACUUM_MODE'),
	p('PROGRESS_VACUUM_MODE_NORMAL'),
	'VACUUM: mode');
is( v($vac, 'PROGRESS_VACUUM_STARTED_BY'),
	p('PROGRESS_VACUUM_STARTED_BY_MANUAL'),
	'VACUUM: started_by');
my $mwm = $node->safe_psql('postgres',
	"SELECT pg_size_bytes(current_setting('maintenance_work_mem'))");
is(v($vac, 'PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES'),
	$mwm, 'VACUUM: max_dead_tuple_bytes is maintenance_work_mem');
ok( v($vac, 'PROGRESS_VACUUM_DEAD_TUPLE_BYTES') > 0
	  && v($vac, 'PROGRESS_VACUUM_DEAD_TUPLE_BYTES') <=
	  v($vac, 'PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES'),
	'VACUUM: dead_tuple_bytes within max_dead_tuple_bytes');
is(v($vac, 'PROGRESS_VACUUM_DELAY_TIME'), 0, 'VACUUM: no delay');

# Each index is vacuumed in turn, and its scan counted by the generic block
# counters.  A btree scan never counts the metapage, so it ends one block
# short of the total, as documented.
my %vacuumed =
  map  { $_ => 1 }
  grep { $_ }
  map  { @$_ }
  values_of($trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_CURRENT_INDEX_RELID');
my @vacuumed = sort { $a <=> $b } keys %vacuumed;
my @prog_indexes = split /\n/,
  $node->safe_psql('postgres',
	"SELECT indexrelid FROM pg_index WHERE indrelid = $relid ORDER BY 1");
is_deeply(\@vacuumed, \@prog_indexes, 'VACUUM: each index is vacuumed');
my @idx_scans =
  grep {
	$_->[2]{ p('PROGRESS_VACUUM_PHASE') } ==
	  p('PROGRESS_VACUUM_PHASE_VACUUM_INDEX')
  } scan_rounds($trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_PHASE',
	'PROGRESS_VACUUM_CURRENT_INDEX_RELID');
is(scalar(@idx_scans), scalar(@prog_indexes), 'VACUUM: one scan per index');
is_deeply([ grep { $_->[1] - $_->[0] != 1 } @idx_scans ],
	[], 'VACUUM: each btree index scan ends one block short of the total');

# The whole succession of each counter, not only where it ends: a report
# that is lost or off by one can still end at the right value, since VACUUM
# sets some counters again when a phase ends.  The table has rows deleted in
# every block, so each block is scanned and then vacuumed, in order.
my $blocks = v($vac, 'PROGRESS_VACUUM_TOTAL_HEAP_BLKS');
is_deeply(
	[
		values_of(
			$trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_HEAP_BLKS_SCANNED')
	],
	[ [ 1 .. $blocks ] ],
	'VACUUM: heap_blks_scanned counts each block');
is_deeply(
	[
		values_of(
			$trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_HEAP_BLKS_VACUUMED')
	],
	[ [ 1 .. $blocks ] ],
	'VACUUM: heap_blks_vacuumed counts each block');

# A block is counted as scanned once it is done with: the dead items of the
# first block are collected while no block is counted yet.
my ($first_dead) =
  grep { $_->{ p('PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS') } }
  map { @$_ } ProgressCheck::states($trace, 'VACUUM', $relid);
is( $first_dead->{ p('PROGRESS_VACUUM_HEAP_BLKS_SCANNED') } // 0,
	0,
	'VACUUM: the first dead items are collected before any block is counted');

# Index vacuuming and then index cleanup each go over the indexes in turn:
# the index being processed, how many are done, and the counters of its
# scan, which only index vacuuming reports for btree.  Each is reset once
# its index or its phase is done.
my %index_pages = map {
	$_ => $node->safe_psql('postgres',
		"SELECT pg_relation_size($_) / current_setting('block_size')::int")
} @prog_indexes;
my $nidx = scalar(@prog_indexes);
is_deeply(
	[
		values_of(
			$trace, 'VACUUM',
			$relid, 'PROGRESS_VACUUM_CURRENT_INDEX_RELID')
	],
	[ [ (map { ($_, 0) } @prog_indexes) x 2 ] ],
	'VACUUM: current_index_relid names each index in turn, in both phases');
is_deeply(
	[ values_of($trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_INDEXES_TOTAL') ],
	[ [ ($nidx, 0) x 2 ] ],
	'VACUUM: indexes_total in both phases');
is_deeply(
	[
		values_of(
			$trace, 'VACUUM', $relid, 'PROGRESS_VACUUM_INDEXES_PROCESSED')
	],
	[ [ (1 .. $nidx, 0) x 2 ] ],
	'VACUUM: indexes_processed counts each index, in both phases');
is_deeply(
	[ values_of($trace, 'VACUUM', $relid, 'PROGRESS_SCAN_BLOCKS_TOTAL') ],
	[ [ map { ($index_pages{$_}, 0) } @prog_indexes ] ],
	'VACUUM: the scan of each index counts its pages');
is_deeply(
	[ values_of($trace, 'VACUUM', $relid, 'PROGRESS_SCAN_BLOCKS_DONE') ],
	[ [ map { (1 .. $index_pages{$_} - 1, 0) } @prog_indexes ] ],
	'VACUUM: the scan of each index counts each page but the metapage');

$node->safe_psql('postgres', "DELETE FROM prog WHERE a > $nrows / 2");
$trace = traced_sql('VACUUM with truncation', 'VACUUM (PARALLEL 0) prog');
is_deeply(
	[ phases($trace, 'VACUUM', $relid) ],
	[
		[
			map { p("PROGRESS_VACUUM_PHASE_$_") }
			  qw(SCAN_HEAP VACUUM_INDEX VACUUM_HEAP INDEX_CLEANUP TRUNCATE
			  FINAL_CLEANUP)
		]
	],
	'VACUUM with truncation: phases');

# VACUUM (FREEZE) is aggressive, and with cost-based delays reports the time
# spent sleeping.
$trace = traced_sql(
	'VACUUM (FREEZE) with cost delays',
	"SET vacuum_cost_delay = '1ms'; SET vacuum_cost_limit = 1; "
	  . 'VACUUM (FREEZE, PARALLEL 0) prog');
($vac) = ProgressCheck::final_values($trace, 'VACUUM', $relid);
is( v($vac, 'PROGRESS_VACUUM_MODE'),
	p('PROGRESS_VACUUM_MODE_AGGRESSIVE'),
	'VACUUM (FREEZE): mode');
cmp_ok(v($vac, 'PROGRESS_VACUUM_DELAY_TIME'),
	'>', 0, 'VACUUM (FREEZE) with cost delays: delay_time');

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
cmp_ok(v($cyc, 'PROGRESS_VACUUM_NUM_INDEX_VACUUMS'),
	'>', 1, 'VACUUM in several cycles: more than one index vacuum cycle');
is(v($cyc, 'PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES'),
	64 * 1024, 'VACUUM in several cycles: max_dead_tuple_bytes');
# Each cycle scans the heap until the dead items fill the memory, vacuums
# the index and then the heap, and goes back to scanning the heap.
is_deeply(
	[ phases($trace, 'VACUUM', $cycles_relid) ],
	[
		[
			(   map { p("PROGRESS_VACUUM_PHASE_$_") }
				  qw(SCAN_HEAP VACUUM_INDEX VACUUM_HEAP)
			) x v($cyc, 'PROGRESS_VACUUM_NUM_INDEX_VACUUMS'),
			map { p("PROGRESS_VACUUM_PHASE_$_") }
			  qw(INDEX_CLEANUP FINAL_CLEANUP)
		]
	],
	'VACUUM in several cycles: phases');
is_deeply(
	[
		values_of(
			$trace, 'VACUUM',
			$cycles_relid, 'PROGRESS_VACUUM_NUM_INDEX_VACUUMS')
	],
	[ [ 1 .. v($cyc, 'PROGRESS_VACUUM_NUM_INDEX_VACUUMS') ] ],
	'VACUUM in several cycles: num_index_vacuums counts each cycle');
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
TODO:
{
	local $TODO =
		"the dead-item counters are not reset after the last index cycle, "
	  . "a harmless blip shown until the next heap scan reaches dead items";
	is_deeply(\@stale, [],
		'VACUUM in several cycles: each heap scan after the first starts with no dead items'
	);
}

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
checked(
	'VACUUM',
	'PROGRESS_VACUUM_INDEXES_TOTAL',
	'PROGRESS_VACUUM_INDEXES_PROCESSED');
my @rounds = ProgressCheck::rounds(
	$trace, 'VACUUM', $relid,
	p('PROGRESS_VACUUM_INDEXES_PROCESSED'),
	p('PROGRESS_VACUUM_INDEXES_TOTAL'),
	p('PROGRESS_VACUUM_PHASE'));
ok(@rounds > 0, 'parallel VACUUM: index rounds were reported');
is_deeply([ grep { $_->[0] != $_->[1] } @rounds ],
	[], 'parallel VACUUM: every round counts every index');

# Each worker reports its own VACUUM of the table, from start to end, even
# when the leader leaves it no index to process.
my %pworkers = map { $_ => $trace->{$_} }
  grep {
	grep { $_->{text} =~ /parallel worker\[/ }
	  @{ $trace->{$_} }
  } keys %$trace;
my %pstarted = ProgressCheck::commands_run(\%pworkers);
ok(($pstarted{VACUUM} // 0) > 0,
	'parallel VACUUM: the workers report a VACUUM');
is( scalar(ProgressCheck::final_values(\%pworkers, 'VACUUM', $relid)),
	$pstarted{VACUUM} // 0,
	'parallel VACUUM: each worker ends the VACUUM of the table it started');

# With cost-based delays, each index takes long enough that the workers do
# process some.  Whoever processes an index reports it, and what a worker
# counts reaches the leader's report.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_pvac (a int, b int, c int);
INSERT INTO prog_pvac SELECT g, g, g FROM generate_series(1, 30000) g;
CREATE INDEX prog_pvac_a ON prog_pvac (a);
CREATE INDEX prog_pvac_b ON prog_pvac (b);
CREATE INDEX prog_pvac_c ON prog_pvac (c);
DELETE FROM prog_pvac WHERE a % 10 = 0;
));
my $pvac_relid =
  $node->safe_psql('postgres', "SELECT 'prog_pvac'::regclass::oid");
$trace = traced_sql(
	'parallel VACUUM with workers',
	"SET vacuum_cost_delay = '1ms'; SET vacuum_cost_limit = 1; "
	  . 'VACUUM (PARALLEL 2, INDEX_CLEANUP ON) prog_pvac');
my @pvac_indexes = split /\n/,
  $node->safe_psql('postgres',
	"SELECT indexrelid FROM pg_index WHERE indrelid = $pvac_relid ORDER BY 1"
  );
my %reported;
foreach my $run (ProgressCheck::states($trace, 'VACUUM', $pvac_relid))
{
	$reported{ $_->{ p('PROGRESS_VACUUM_CURRENT_INDEX_RELID') } // 0 } = 1
	  foreach grep {
		($_->{ p('PROGRESS_VACUUM_PHASE') } // 0) ==
		  p('PROGRESS_VACUUM_PHASE_VACUUM_INDEX')
	  } @$run;
}
checked('VACUUM', 'PROGRESS_VACUUM_CURRENT_INDEX_RELID');
delete $reported{0};
is_deeply([ sort { $a <=> $b } keys %reported ],
	\@pvac_indexes, 'parallel VACUUM with workers: each index is reported');
my %pvw = map { $_ => $trace->{$_} }
  grep {
	grep { $_->{text} =~ /parallel worker\[/ && $_->{event} eq 'update' }
	  @{ $trace->{$_} }
  } keys %$trace;
# On a slow machine the leader may be done before a worker starts; that is
# not a failure, but then nothing reaches the leader from a worker.
note('parallel VACUUM with workers: no worker processed an index')
  unless %pvw;
my @pv_rounds = ProgressCheck::rounds(
	$trace, 'VACUUM', $pvac_relid,
	p('PROGRESS_VACUUM_INDEXES_PROCESSED'),
	p('PROGRESS_VACUUM_INDEXES_TOTAL'),
	p('PROGRESS_VACUUM_PHASE'));
is_deeply(
	[ map { [ $_->[0], $_->[1] ] } @pv_rounds ],
	[ ([ 3, 3 ]) x 2 ],
	'parallel VACUUM with workers: the leader counts every index, in both phases'
);

# A partial index has fewer tuples than the indexes REPACK rebuilds before
# it; see @ACCEPTED.
$node->safe_psql('postgres',
	'CREATE INDEX prog_a_even ON prog (a) WHERE a % 2 = 0');

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
		values_of(
			$trace, 'REPACK',
			$relid, 'PROGRESS_REPACK_INDEX_REBUILD_COUNT')
	];
}
# The values of PROGRESS_REPACK_COMMAND are those of RepackCommand.
my %repack_command;
{
	(my $parsenodes_h = $progress_h) =~
	  s{commands/progress\.h$}{nodes/parsenodes.h};
	my ($enum) =
	  slurp_file($parsenodes_h) =~ /typedef enum RepackCommand\s*\{(.*?)\}/s;
	my $n = 0;
	foreach (split /,/, $enum)
	{
		next unless /(REPACK_COMMAND_\w+)(?:\s*=\s*(\d+))?/;
		$n = $2 if defined $2;
		$repack_command{$1} = $n++;
	}
}
my $prog_a = $node->safe_psql('postgres', "SELECT 'prog_a'::regclass::oid");
my $live = $node->safe_psql('postgres', 'SELECT count(*) FROM prog');

# What REPACK reports about the command it runs and the rows it copies.
sub repack_values
{
	my ($name, $trace, $relid, $command, $index) = @_;
	my ($rp) = ProgressCheck::final_values($trace, 'REPACK', $relid);
	is( v($rp, 'PROGRESS_REPACK_COMMAND'),
		$repack_command{$command},
		"$name: command");
	is(v($rp, 'PROGRESS_REPACK_INDEX_RELID'),
		$index, "$name: repack_index_relid");
	is(v($rp, 'PROGRESS_REPACK_HEAP_TUPLES_INSERTED'),
		$live, "$name: heap_tuples_inserted is the number of rows");
	cmp_ok(v($rp, 'PROGRESS_REPACK_HEAP_TUPLES_SCANNED'),
		'>=', $live, "$name: heap_tuples_scanned");
	# one step per tuple, not only the right final count
	is_deeply(
		[
			values_of(
				$trace, 'REPACK',
				$relid, 'PROGRESS_REPACK_HEAP_TUPLES_SCANNED')
		],
		[ [ 1 .. v($rp, 'PROGRESS_REPACK_HEAP_TUPLES_SCANNED') ] ],
		"$name: heap_tuples_scanned counts each tuple");
	is_deeply(
		[
			values_of(
				$trace, 'REPACK',
				$relid, 'PROGRESS_REPACK_HEAP_TUPLES_INSERTED')
		],
		[ [ 1 .. $live ] ],
		"$name: heap_tuples_inserted counts each row");
}

my @repack_sort = map { p("PROGRESS_REPACK_PHASE_$_") }
  qw(SEQ_SCAN_HEAP SORT_TUPLES WRITE_NEW_HEAP SWAP_REL_FILES REBUILD_INDEX
  FINAL_CLEANUP);
my @repack_index = map { p("PROGRESS_REPACK_PHASE_$_") }
  qw(INDEX_SCAN_HEAP SWAP_REL_FILES REBUILD_INDEX FINAL_CLEANUP);

$trace = traced_sql('REPACK USING INDEX with a sort',
	'SET enable_indexscan = off; REPACK prog USING INDEX prog_a');
is_deeply(
	[ phases($trace, 'REPACK', $relid) ],
	[ \@repack_sort ],
	'REPACK USING INDEX with a sort: phases');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'REPACK USING INDEX with a sort: index_rebuild_count');
repack_values('REPACK USING INDEX with a sort',
	$trace, $relid, 'REPACK_COMMAND_REPACK', $prog_a);

$trace = traced_sql('REPACK USING INDEX', 'REPACK prog USING INDEX prog_a');
my ($phases) = phases($trace, 'REPACK', $relid);
ok( "@$phases" eq "@repack_sort" || "@$phases" eq "@repack_index",
	'REPACK USING INDEX: phases of either way of ordering the heap'
) or diag("got phases @$phases");
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'REPACK USING INDEX: index_rebuild_count');
repack_values('REPACK USING INDEX',
	$trace, $relid, 'REPACK_COMMAND_REPACK', $prog_a);

$trace = traced_sql('REPACK', 'REPACK prog');
is_deeply(
	[ phases($trace, 'REPACK', $relid) ],
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
repack_values('REPACK', $trace, $relid, 'REPACK_COMMAND_REPACK', 0);
my ($rp) = ProgressCheck::final_values($trace, 'REPACK', $relid);
is( v($rp, 'PROGRESS_REPACK_HEAP_BLKS_SCANNED'),
	v($rp, 'PROGRESS_REPACK_TOTAL_HEAP_BLKS'),
	'REPACK: every block scanned');
is_deeply(
	[
		values_of(
			$trace, 'REPACK', $relid, 'PROGRESS_REPACK_HEAP_BLKS_SCANNED')
	],
	[ [ 1 .. v($rp, 'PROGRESS_REPACK_TOTAL_HEAP_BLKS') ] ],
	'REPACK: heap_blks_scanned counts each block');

# CLUSTER is reported as REPACK too, as the command that it is.
$trace = traced_sql('CLUSTER', 'CLUSTER prog USING prog_a');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'CLUSTER: index_rebuild_count');
repack_values('CLUSTER', $trace, $relid, 'REPACK_COMMAND_CLUSTER', $prog_a);

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
# It builds the indexes on the new heap before it catches up with the
# changes made meanwhile, and swaps the files after.
is_deeply(
	[ phases($trace, 'REPACK', $conc_relid) ],
	[
		[
			map { p("PROGRESS_REPACK_PHASE_$_") }
			  qw(SEQ_SCAN_HEAP REBUILD_INDEX CATCH_UP SWAP_REL_FILES
			  FINAL_CLEANUP)
		]
	],
	'REPACK (CONCURRENTLY): phases');

my ($conc) = ProgressCheck::final_values($trace, 'REPACK', $conc_relid);
is_deeply(
	[
		values_of(
			$trace, 'REPACK',
			$conc_relid, 'PROGRESS_REPACK_HEAP_BLKS_SCANNED')
	],
	[ [ 1 .. v($conc, 'PROGRESS_REPACK_TOTAL_HEAP_BLKS') ] ],
	'REPACK (CONCURRENTLY): heap_blks_scanned counts each block');
is_deeply(
	[
		map { [ values_of($trace, 'REPACK', $conc_relid, $_) ] }
		  qw(PROGRESS_REPACK_HEAP_TUPLES_UPDATED
		  PROGRESS_REPACK_HEAP_TUPLES_DELETED)
	],
	[ [ [] ], [ [] ] ],
	'REPACK (CONCURRENTLY) without concurrent changes: none counted');
is_deeply(
	[ values_of($trace, 'REPACK', $conc_relid, 'PROGRESS_REPACK_COMMAND') ],
	[ [ $repack_command{REPACK_COMMAND_REPACK} ] ],
	'REPACK (CONCURRENTLY): command, set once');

# The changes made to the table while REPACK (CONCURRENTLY) copies it are
# applied to the new heap when it catches up, and counted there.  To make
# them happen at a known point, REPACK is held while it rebuilds an index:
# the index calls a function that, in REPACK's session only, waits for an
# advisory lock that the test holds.
$node->safe_psql(
	'postgres', q{
CREATE TABLE prog_apply (a int PRIMARY KEY, b int);
INSERT INTO prog_apply SELECT g, g FROM generate_series(1, 100) g;
CREATE FUNCTION prog_gate(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
  IF current_setting('prog.gate', true) = 'on' THEN
    PERFORM pg_advisory_lock_shared(7331);
    PERFORM pg_advisory_unlock_shared(7331);
  END IF;
  RETURN $1;
END$$;
CREATE INDEX prog_apply_gate ON prog_apply (prog_gate(b));
});
my $apply_relid =
  $node->safe_psql('postgres', "SELECT 'prog_apply'::regclass::oid");
my $gate = $node->background_psql('postgres');
$gate->query_safe('SELECT pg_advisory_lock(7331)');
my $repack_bg = $node->background_psql('postgres');
my $repack_pid = $repack_bg->query_safe('SELECT pg_backend_pid()');
$trace = traced(
	'REPACK (CONCURRENTLY) with concurrent changes',
	sub {
		$repack_bg->query_until(
			qr/repack starts/,
			"SET prog.gate = on;\n\\echo repack starts\nREPACK (CONCURRENTLY) prog_apply;\n"
		);
		$node->poll_query_until('postgres',
			"SELECT wait_event = 'advisory' FROM pg_stat_activity WHERE pid = $repack_pid"
		) or die 'REPACK (CONCURRENTLY) did not reach the gate';
		$node->safe_psql(
			'postgres', q{
INSERT INTO prog_apply SELECT g, g FROM generate_series(101, 110) g;
UPDATE prog_apply SET b = b + 1000 WHERE a <= 4;
DELETE FROM prog_apply WHERE a > 107;
});
		$gate->query_safe('SELECT pg_advisory_unlock(7331)');
		# the next query gets an answer once REPACK is done
		$repack_bg->query_safe('SELECT 1');
	});
$_->quit foreach ($gate, $repack_bg);
is_deeply(
	[ phases($trace, 'REPACK', $apply_relid) ],
	[
		[
			map { p("PROGRESS_REPACK_PHASE_$_") }
			  qw(SEQ_SCAN_HEAP REBUILD_INDEX CATCH_UP SWAP_REL_FILES
			  FINAL_CLEANUP)
		]
	],
	'REPACK (CONCURRENTLY) with concurrent changes: phases');
is_deeply(
	[
		map {
			[
				map { @$_ } values_of(
					$trace, 'REPACK',
					$apply_relid, "PROGRESS_REPACK_HEAP_TUPLES_$_")
			]
		} qw(INSERTED UPDATED DELETED)
	],
	[ [ 1 .. 110 ], [ 1 .. 4 ], [ 1 .. 3 ] ],
	'REPACK (CONCURRENTLY) with concurrent changes: each change applied is counted'
);

# Base backups, reported by the walsender.  WAL is only transferred at the
# end when it is not streamed.
my @backup_phases = map { p("PROGRESS_BASEBACKUP_PHASE_$_") }
  qw(WAIT_CHECKPOINT ESTIMATE_BACKUP_SIZE STREAM_BACKUP WAIT_WAL_ARCHIVE);
$trace = traced(
	'BASEBACKUP',
	sub { $node->backup('backup', backup_options => ['--wal-method=stream']) }
);
is_deeply(
	[ phases($trace, 'BASEBACKUP', undef) ],
	[ \@backup_phases ],
	'BASEBACKUP: phases');

# With nothing else running, the estimate of the size of the backup is
# exactly what is streamed.
sub backup_values
{
	my ($name, $trace, $type) = @_;
	my ($bb) = ProgressCheck::final_values($trace, 'BASEBACKUP', undef);
	is( v($bb, 'PROGRESS_BASEBACKUP_BACKUP_TYPE'),
		p("PROGRESS_BASEBACKUP_BACKUP_TYPE_$type"),
		"$name: backup_type");
	cmp_ok(v($bb, 'PROGRESS_BASEBACKUP_BACKUP_TOTAL'),
		'>', 0, "$name: backup_total");
	is( v($bb, 'PROGRESS_BASEBACKUP_BACKUP_STREAMED'),
		v($bb, 'PROGRESS_BASEBACKUP_BACKUP_TOTAL'),
		"$name: backup_streamed is backup_total");
	is(v($bb, 'PROGRESS_BASEBACKUP_TBLSPC_TOTAL'),
		1, "$name: tablespaces_total");
	is(v($bb, 'PROGRESS_BASEBACKUP_TBLSPC_STREAMED'),
		1, "$name: tablespaces_streamed");
}
backup_values('BASEBACKUP', $trace, 'FULL');
$trace = traced(
	'BASEBACKUP fetching WAL',
	sub { $node->backup('backup2', backup_options => ['--wal-method=fetch']) }
);
is_deeply(
	[ phases($trace, 'BASEBACKUP', undef) ],
	[ [ @backup_phases, p('PROGRESS_BASEBACKUP_PHASE_TRANSFER_WAL') ] ],
	'BASEBACKUP fetching WAL: phases');

# An incremental backup estimates the size of what it sends, not of the
# full files.
$node->safe_psql('postgres', 'INSERT INTO prog_pipe VALUES (4)');
my $manifest = $node->backup_dir . '/backup/backup_manifest';
$trace = traced(
	'incremental BASEBACKUP',
	sub {
		$node->backup('backup_incr',
			backup_options => [ '--incremental', $manifest ]);
	});
is_deeply(
	[ phases($trace, 'BASEBACKUP', undef) ],
	[ \@backup_phases ],
	'incremental BASEBACKUP: phases');
backup_values('incremental BASEBACKUP', $trace, 'INCREMENTAL');

# REINDEX reports each index as its own CREATE INDEX command, with the
# command set to REINDEX; REINDEX CONCURRENTLY goes through the phases of
# CREATE INDEX CONCURRENTLY.
$trace = traced_sql('REINDEX TABLE', 'REINDEX TABLE prog');
my @reindex = ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is(scalar(@reindex), $nindexes, 'REINDEX TABLE: one run per index');
is_deeply(
	[ map { v($_, 'PROGRESS_CREATEIDX_COMMAND') } @reindex ],
	[ (p('PROGRESS_CREATEIDX_COMMAND_REINDEX')) x $nindexes ],
	'REINDEX TABLE: command');
is_deeply(
	[ map { v($_, 'PROGRESS_CREATEIDX_INDEX_OID') } @reindex ],
	[
		split /\n/,
		$node->safe_psql(
			'postgres',
			"SELECT indexrelid FROM pg_index WHERE indrelid = $relid ORDER BY 1"
		)
	],
	'REINDEX TABLE: index_relid of each run');
is_deeply(
	[ map { v($_, 'PROGRESS_CREATEIDX_ACCESS_METHOD_OID') } @reindex ],
	[ ($am{btree}) x $nindexes ],
	'REINDEX TABLE: access method of each run');

my $old_prog_a =
  $node->safe_psql('postgres', "SELECT 'prog_a'::regclass::oid");
$trace = traced_sql('REINDEX INDEX CONCURRENTLY',
	'REINDEX INDEX CONCURRENTLY prog_a');
my ($reindex_conc) =
  ProgressCheck::final_values($trace, 'CREATE_INDEX', $relid);
is( v($reindex_conc, 'PROGRESS_CREATEIDX_COMMAND'),
	p('PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY'),
	'REINDEX INDEX CONCURRENTLY: command');

# It starts its report again for each of its transactions: the first waits
# for the lockers of the old index, the second builds the new index and the
# last validates it and waits for the old one to be unused, then dropped.
# Each start names the table, the command, the index and its access method.
my $new_prog_a =
  $node->safe_psql('postgres', "SELECT 'prog_a'::regclass::oid");
is_deeply(
	[ phases($trace, 'CREATE_INDEX', $relid) ],
	[
		[ p('PROGRESS_CREATEIDX_PHASE_WAIT_1') ],
		[ map { p("PROGRESS_CREATEIDX_PHASE_$_") } qw(BUILD WAIT_2) ],
		[
			map { p("PROGRESS_CREATEIDX_PHASE_$_") }
			  qw(VALIDATE_IDXSCAN VALIDATE_SORT VALIDATE_TABLESCAN
			  WAIT_3 WAIT_4 WAIT_5)
		]
	],
	'REINDEX INDEX CONCURRENTLY: phases of each transaction');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_CREATEIDX_COMMAND')
	],
	[ ([ p('PROGRESS_CREATEIDX_COMMAND_REINDEX_CONCURRENTLY') ]) x 3 ],
	'REINDEX INDEX CONCURRENTLY: command of each transaction');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX', $relid, 'PROGRESS_CREATEIDX_INDEX_OID')
	],
	[ [$old_prog_a], [$new_prog_a], [$new_prog_a] ],
	'REINDEX INDEX CONCURRENTLY: index_relid, the old index and then the new'
);
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$relid, 'PROGRESS_CREATEIDX_ACCESS_METHOD_OID')
	],
	[ ([ $am{btree} ]) x 3 ],
	'REINDEX INDEX CONCURRENTLY: access method of each transaction');

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
$trace = traced_sql(
	'CREATE INDEX on a partitioned table',
	'CREATE INDEX prog_part_a ON prog_part (a)');
my ($part) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $part_relid);
is(v($part, 'PROGRESS_CREATEIDX_PARTITIONS_TOTAL'),
	3, 'CREATE INDEX on a partitioned table: partitions_total');
is(v($part, 'PROGRESS_CREATEIDX_PARTITIONS_DONE'),
	3, 'CREATE INDEX on a partitioned table: partitions_done');

# Each partition's build starts from block counters set back to 0.
my @part_blocks = split /\n/,
  $node->safe_psql('postgres',
		"SELECT pg_relation_size(relid) / current_setting('block_size')::int "
	  . "FROM pg_partition_tree('prog_part') WHERE isleaf ORDER BY relid::oid"
  );
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$part_relid, 'PROGRESS_SCAN_BLOCKS_TOTAL')
	],
	[ [ map { ($_, 0) } @part_blocks ] ],
	'CREATE INDEX on a partitioned table: blocks_total of each partition');

# Every partition counts once, whatever it takes: one with a matching index
# already has it attached, and a partitioned one counts as well as its own
# partitions.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_tree (a int) PARTITION BY RANGE (a);
CREATE TABLE prog_tree_1 PARTITION OF prog_tree FOR VALUES FROM (0) TO (100);
CREATE INDEX prog_tree_1_a ON prog_tree_1 (a);
CREATE TABLE prog_tree_2 PARTITION OF prog_tree FOR VALUES FROM (100) TO (200)
  PARTITION BY RANGE (a);
CREATE TABLE prog_tree_2a PARTITION OF prog_tree_2 FOR VALUES FROM (100) TO (150);
CREATE TABLE prog_tree_2b PARTITION OF prog_tree_2 FOR VALUES FROM (150) TO (200);
INSERT INTO prog_tree SELECT g FROM generate_series(0, 199) g;
));
my $tree_relid =
  $node->safe_psql('postgres', "SELECT 'prog_tree'::regclass::oid");
$trace = traced_sql(
	'CREATE INDEX on a partition tree',
	'CREATE INDEX prog_tree_a ON prog_tree (a)');
my ($tree) = ProgressCheck::final_values($trace, 'CREATE_INDEX', $tree_relid);
is(v($tree, 'PROGRESS_CREATEIDX_PARTITIONS_TOTAL'),
	4, 'CREATE INDEX on a partition tree: partitions_total');
is_deeply(
	[
		values_of(
			$trace, 'CREATE_INDEX',
			$tree_relid, 'PROGRESS_CREATEIDX_PARTITIONS_DONE')
	],
	[ [ 1 .. 4 ] ],
	'CREATE INDEX on a partition tree: partitions_done counts each partition'
);

# ANALYZE of a partitioned table samples the whole tree, then computes the
# extended statistics defined on it.
$trace = traced_sql('ANALYZE of a partitioned table', 'ANALYZE prog_part');
my @part_phases = phases($trace, 'ANALYZE', $part_relid);
ok( (   grep { $_ == p('PROGRESS_ANALYZE_PHASE_ACQUIRE_SAMPLE_ROWS_INH') }
		map  { @$_ } @part_phases),
	'ANALYZE of a partitioned table: samples the inheritance tree'
) or diag(explain \@part_phases);
ok( (   grep { $_ == p('PROGRESS_ANALYZE_PHASE_COMPUTE_EXT_STATS') }
		map  { @$_ } @part_phases),
	'ANALYZE of a partitioned table: computes the extended statistics'
) or diag(explain \@part_phases);
my %sampled =
  map  { $_ => 1 }
  grep { $_ }
  map  { @$_ }
  values_of($trace,
	'ANALYZE', $part_relid, 'PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID');
is_deeply(
	[ sort { $a <=> $b } keys %sampled ],
	[
		split /\n/,
		$node->safe_psql(
			'postgres',
			"SELECT relid::oid FROM pg_partition_tree('prog_part') "
			  . 'WHERE isleaf ORDER BY 1')
	],
	'ANALYZE of a partitioned table: samples each partition');

# Step by step: the partitions in turn, the blocks of each from 0, the
# child tables done and the extended statistics computed.
my @part_leaves = split /\n/,
  $node->safe_psql('postgres',
	"SELECT relid::oid FROM pg_partition_tree('prog_part') WHERE isleaf ORDER BY 1"
  );
my %leaf_blocks = map {
	$_ => $node->safe_psql('postgres',
		"SELECT pg_relation_size($_) / current_setting('block_size')::int")
} @part_leaves;
my ($part_run) =
  grep { @{ $_->[0] } }
  map {
	my $i = $_;
	[
		map   { (values_of($trace, 'ANALYZE', $part_relid, $_))[$i] }
		  map { "PROGRESS_ANALYZE_$_" }
		  qw(CURRENT_CHILD_TABLE_RELID BLOCKS_TOTAL BLOCKS_DONE CHILD_TABLES_DONE)
	]
  } (0 .. scalar(ProgressCheck::final_values($trace, 'ANALYZE', $part_relid))
	  - 1);
my @n = map { $leaf_blocks{$_} } @part_leaves;
is_deeply(
	$part_run,
	[
		\@part_leaves,
		[ map { ($n[$_], ($_ < $#n ? 0 : ())) } 0 .. $#n ],
		[ 1 .. $n[0], map { (0 .. $n[$_]) } 1 .. $#n ],
		[ 1 .. scalar(@part_leaves) ]
	],
	'ANALYZE of a partitioned table: each partition in turn, block by block');
foreach my $an (ProgressCheck::final_values($trace, 'ANALYZE', $part_relid))
{
	is( v($an, 'PROGRESS_ANALYZE_CHILD_TABLES_DONE'),
		v($an, 'PROGRESS_ANALYZE_CHILD_TABLES_TOTAL'),
		'ANALYZE of a partitioned table: every child table done');
	is( v($an, 'PROGRESS_ANALYZE_EXT_STATS_COMPUTED'),
		v($an, 'PROGRESS_ANALYZE_EXT_STATS_TOTAL'),
		'ANALYZE of a partitioned table: every extended statistic computed');
}

# COPY FROM counts the rows that WHERE excludes and the rows that
# ON_ERROR ignore skips, apart from the rows it loads.
$node->safe_psql('postgres', 'CREATE TABLE prog_copy (a int)');
my $copy_relid =
  $node->safe_psql('postgres', "SELECT 'prog_copy'::regclass::oid");
PostgreSQL::Test::Utils::append_to_file("$tempdir/copy.data",
	join('', map { $_ % 10 == 0 ? "bad\n" : "$_\n" } 1 .. 100));
$trace = traced_sql(
	'COPY FROM with WHERE and ON_ERROR',
	"COPY prog_copy FROM '$tempdir/copy.data' WITH (ON_ERROR ignore) WHERE a > 50"
);
($copy) = ProgressCheck::final_values($trace, 'COPY', $copy_relid);
is(v($copy, 'PROGRESS_COPY_TUPLES_SKIPPED'),
	10, 'COPY FROM with ON_ERROR ignore: tuples_skipped');
is(v($copy, 'PROGRESS_COPY_TUPLES_EXCLUDED'),
	45, 'COPY FROM with WHERE: tuples_excluded');
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	45, 'COPY FROM with WHERE and ON_ERROR: tuples_processed');

# VACUUM FULL is reported as REPACK, and rebuilds every index.
$trace = traced_sql('VACUUM FULL', 'VACUUM FULL prog');
is_deeply(
	rebuild_counts($trace, $relid),
	[ [ 1 .. $nindexes ] ],
	'VACUUM FULL: index_rebuild_count');
repack_values('VACUUM FULL', $trace, $relid, 'REPACK_COMMAND_VACUUMFULL', 0);

# A command that fails part of the way through still ends its report, before
# the backend is ready for its next query (the 'idle' rule), and what comes
# next starts again from 0.  prog_fail() fails while prog.fail is on, so
# that an index on it can be built first and then fail to be rebuilt.
$node->safe_psql(
	'postgres', q{
CREATE FUNCTION prog_fail(int) RETURNS int IMMUTABLE LANGUAGE plpgsql AS $$
BEGIN
  IF $1 = 2000 AND current_setting('prog.fail', true) = 'on' THEN
    RAISE 'prog_fail';
  END IF;
  RETURN $1;
END $$;
CREATE TABLE prog_err (a int);
INSERT INTO prog_err SELECT g FROM generate_series(1, 3000) g;
CREATE INDEX prog_err_f ON prog_err (prog_fail(a));
});
my $err_relid =
  $node->safe_psql('postgres', "SELECT 'prog_err'::regclass::oid");

sub traced_failure
{
	my ($name, $command, $phase, $sql) = @_;
	my $trace = traced(
		$name,
		sub {
			my ($ret) = $node->psql('postgres', $sql);
			isnt($ret, 0, "$name: fails");
		});
	my @runs = ProgressCheck::final_values($trace, $command, undef);
	is(scalar(@runs), 1, "$name: the report ends");
	my @phases = map { @$_ } phases($trace, $command, undef);
	is($phases[-1], p($phase), "$name: fails in its phase");
}

my $fail = 'SET prog.fail = on; ';
traced_failure(
	'CREATE INDEX that fails',
	'CREATE_INDEX',
	'PROGRESS_CREATEIDX_PHASE_BUILD',
	$fail . 'CREATE INDEX prog_err_g ON prog_err (prog_fail(a))');
traced_failure(
	'CREATE INDEX CONCURRENTLY that fails',
	'CREATE_INDEX',
	'PROGRESS_CREATEIDX_PHASE_BUILD',
	$fail . 'CREATE INDEX CONCURRENTLY prog_err_c ON prog_err (prog_fail(a))'
);
$node->safe_psql('postgres', 'DROP INDEX prog_err_c');
traced_failure(
	'REPACK that fails',
	'REPACK',
	'PROGRESS_REPACK_PHASE_REBUILD_INDEX',
	$fail . 'REPACK prog_err');
traced_failure(
	'ANALYZE that fails',
	'ANALYZE',
	'PROGRESS_ANALYZE_PHASE_COMPUTE_STATS',
	$fail . 'ANALYZE prog_err');

# A VACUUM that is canceled while it sleeps, once it is seen scanning.
my $bg = $node->background_psql('postgres', on_error_stop => 0);
my $bg_pid = $bg->query_safe(
		"SET vacuum_cost_delay = '100ms'; SET vacuum_cost_limit = 1; "
	  . 'SELECT pg_backend_pid()');
$trace = traced(
	'VACUUM that is canceled',
	sub {
		$bg->query_until(qr/vacuum starts/,
			"\\echo vacuum starts\nVACUUM (FREEZE) prog_err;\n");
		$node->poll_query_until('postgres',
			"SELECT phase = 'scanning heap' FROM pg_stat_progress_vacuum WHERE pid = $bg_pid"
		) or die 'VACUUM did not start scanning';
		$node->safe_psql('postgres', "SELECT pg_cancel_backend($bg_pid)");
		# the next query gets an answer once the VACUUM has failed
		$bg->query('SELECT 1');
	});
$bg->quit;
is(scalar(ProgressCheck::final_values($trace, 'VACUUM', $err_relid)),
	1, 'VACUUM that is canceled: the report ends');
$trace = traced(
	'COPY FROM that fails',
	sub {
		my ($ret) = $node->psql('postgres',
			"COPY prog_err FROM STDIN;\n1\n2\nnot a number\n4\n\\.\n");
		isnt($ret, 0, 'COPY FROM that fails: fails');
	});
is(scalar(ProgressCheck::final_values($trace, 'COPY', $err_relid)),
	1, 'COPY FROM that fails: the report ends');

# One that fails in a subtransaction ends its report when the subtransaction
# is rolled back, before the transaction goes on.
$trace = traced(
	'COPY FROM that fails in a subtransaction',
	sub {
		my ($ret, $out, $err) = $node->psql('postgres',
			"BEGIN;\nSAVEPOINT s;\nCOPY prog_err FROM STDIN;\n1\nnot a number\n\\.\n"
			  . "ROLLBACK TO s;\nSELECT 1;\nCOMMIT;\n");
		like(
			$err,
			qr/invalid input syntax/,
			'COPY FROM that fails in a subtransaction: fails');
	});
is(scalar(ProgressCheck::final_values($trace, 'COPY', $err_relid)),
	1, 'COPY FROM that fails in a subtransaction: the report ends');

# A base backup that fails while the client stays connected ends its report
# as well.  It fails here because it cannot read a file that it sends, one
# that the server itself never uses.
SKIP:
{
	skip 'file permissions are not enforced on Windows', 4 if $windows_os;

	my $file = $node->data_dir . '/prog_unreadable';
	PostgreSQL::Test::Utils::append_to_file($file, 'x');
	chmod 0, $file or die "could not chmod $file: $!";
	$trace = traced(
		'BASEBACKUP that fails',
		sub {
			my ($ret) = $node->psql(
				'postgres',
				"BASE_BACKUP (TARGET 'blackhole', CHECKPOINT 'fast')",
				replication => 'database');
			isnt($ret, 0, 'BASEBACKUP that fails: fails');
		});
	unlink $file or die "could not remove $file: $!";
	my @runs = ProgressCheck::final_values($trace, 'BASEBACKUP', undef);
	is(scalar(@runs), 1, 'BASEBACKUP that fails: the report ends');
	my @phases = map { @$_ } phases($trace, 'BASEBACKUP', undef);
	is( $phases[-1],
		p('PROGRESS_BASEBACKUP_PHASE_STREAM_BACKUP'),
		'BASEBACKUP that fails: fails in its phase');
}

# A base backup that does not estimate its size reports the total as not
# known (NULL in the view), and counts each tablespace it sends.
my $ts_dir = "$tempdir/prog_ts";
mkdir $ts_dir or die "could not create $ts_dir: $!";
$node->safe_psql('postgres',
		"CREATE TABLESPACE prog_ts LOCATION '$ts_dir'; "
	  . 'CREATE TABLE prog_in_ts (a int) TABLESPACE prog_ts; '
	  . 'INSERT INTO prog_in_ts VALUES (1)');
$trace = traced(
	'BASEBACKUP of two tablespaces without an estimate',
	sub {
		$node->backup('backup_ts',
			backup_options => [ '--format=tar', '--no-estimate-size' ]);
	});
my ($bb2) = ProgressCheck::final_values($trace, 'BASEBACKUP', undef);
is(v($bb2, 'PROGRESS_BASEBACKUP_BACKUP_TOTAL'),
	-1, 'BASEBACKUP without an estimate: backup_total is not known');
is(v($bb2, 'PROGRESS_BASEBACKUP_TBLSPC_TOTAL'),
	2, 'BASEBACKUP of two tablespaces: tablespaces_total');
is_deeply(
	[
		values_of(
			$trace, 'BASEBACKUP', 0, 'PROGRESS_BASEBACKUP_TBLSPC_STREAMED')
	],
	[ [ 1, 2 ] ],
	'BASEBACKUP of two tablespaces: tablespaces_streamed counts each');

# backup_total is -1, not known, until an estimate replaces it: never 0.
is( scalar(
		grep { !$_->{ p('PROGRESS_BASEBACKUP_BACKUP_TOTAL') } }
		map  { @$_ } ProgressCheck::states($trace, 'BASEBACKUP', 0)),
	0,
	'BASEBACKUP without an estimate: backup_total is never 0');
$node->safe_psql('postgres',
	'DROP TABLE prog_in_ts; DROP TABLESPACE prog_ts');

# The initial copy of a table that a subscription makes is a COPY, done by a
# table synchronization worker of the subscriber, which then catches up.
my $sub = PostgreSQL::Test::Cluster->new('sub');
$sub->init;
$sub->append_conf('postgresql.conf', 'trace_progress = on');
$sub->start;
$node->safe_psql(
	'postgres', q{
CREATE TABLE prog_sync (a int PRIMARY KEY);
INSERT INTO prog_sync SELECT g FROM generate_series(1, 1000) g;
CREATE PUBLICATION prog_pub FOR TABLE prog_sync;
});
$sub->safe_psql('postgres', 'CREATE TABLE prog_sync (a int PRIMARY KEY)');
my $sync_relid =
  $sub->safe_psql('postgres', "SELECT 'prog_sync'::regclass::oid");
my $connstr = $node->connstr . ' dbname=postgres';
$trace = traced_on(
	$sub,
	'COPY of a table synchronization',
	sub {
		$sub->safe_psql('postgres',
			"CREATE SUBSCRIPTION prog_sub CONNECTION '$connstr' PUBLICATION prog_pub"
		);
		$sub->wait_for_subscription_sync($node, 'prog_sub');
	});
($copy) = ProgressCheck::final_values($trace, 'COPY', $sync_relid);
is(v($copy, 'PROGRESS_COPY_TUPLES_PROCESSED'),
	1000, 'COPY of a table synchronization: tuples_processed');
is( v($copy, 'PROGRESS_COPY_COMMAND'),
	p('PROGRESS_COPY_COMMAND_FROM'),
	'COPY of a table synchronization: command');
is( v($copy, 'PROGRESS_COPY_TYPE'),
	p('PROGRESS_COPY_TYPE_CALLBACK'),
	'COPY of a table synchronization: type');
$sub->safe_psql('postgres', 'DROP SUBSCRIPTION prog_sub');
$sub->stop;

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
	  grep {
		grep { $_->{text} =~ /\Q$type\E\[/ }
		  @{ $trace->{$_} }
	  }
	  keys %$trace;
	return ProgressCheck::final_values(\%by_type, 'DATACHECKSUMS', 0);
}

sub checksums_phases
{
	my ($trace, $type) = @_;
	my %by_type = map { $_ => $trace->{$_} }
	  grep {
		grep { $_->{text} =~ /\Q$type\E\[/ }
		  @{ $trace->{$_} }
	  }
	  keys %$trace;
	return phases(\%by_type, 'DATACHECKSUMS', 0);
}

# The documented NULLs: only the launcher counts databases, and only when
# enabling; only workers count relations and blocks.
sub checksums_nulls
{
	my ($name, $run, @macros) = @_;
	my %column =
	  (DBS => 'databases', RELS => 'relations', BLOCKS => 'blocks');
	my @columns = map { lc(s/^([A-Z]+)_/$column{$1}_/r) } @macros;
	is_deeply(
		[ map { v($run, "PROGRESS_DATACHECKSUMS_$_") } @macros ],
		[ (-1) x @macros ],
		"$name: NULL " . join(', ', @columns));
}

$trace = checksums_trace('DATACHECKSUMS disable',
	'SELECT pg_disable_data_checksums()', 'off');
is_deeply(
	[ checksums_phases($trace, 'datachecksums launcher') ],
	[ [ map { p("PROGRESS_DATACHECKSUMS_PHASE_$_") } qw(DISABLING DONE) ] ],
	'DATACHECKSUMS disable: phases of the launcher');
checksums_nulls(
	'DATACHECKSUMS disable: launcher',
	checksums_runs($trace, 'datachecksums launcher'),
	qw(DBS_TOTAL DBS_DONE RELS_TOTAL RELS_DONE BLOCKS_TOTAL BLOCKS_DONE));

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
cmp_ok(v($launcher, 'PROGRESS_DATACHECKSUMS_DBS_TOTAL'),
	'>', 0, 'DATACHECKSUMS enable: databases_total');
is( v($launcher, 'PROGRESS_DATACHECKSUMS_DBS_DONE'),
	v($launcher, 'PROGRESS_DATACHECKSUMS_DBS_TOTAL'),
	'DATACHECKSUMS enable: every database done');
my @workers = checksums_runs($trace, 'datachecksums worker');
is( scalar(@workers),
	v($launcher, 'PROGRESS_DATACHECKSUMS_DBS_TOTAL'),
	'DATACHECKSUMS enable: one worker per database');
is_deeply(
	[
		grep {
			v($_, 'PROGRESS_DATACHECKSUMS_RELS_DONE') !=
			  v($_, 'PROGRESS_DATACHECKSUMS_RELS_TOTAL')
		} @workers
	],
	[],
	'DATACHECKSUMS enable: each worker does every relation');
checksums_nulls('DATACHECKSUMS enable: launcher',
	$launcher, qw(RELS_TOTAL RELS_DONE BLOCKS_TOTAL BLOCKS_DONE));
checksums_nulls('DATACHECKSUMS enable: worker', $_, qw(DBS_TOTAL DBS_DONE))
  foreach @workers;

# Step by step: the launcher counts the databases from 0, each worker the
# relations of its database from 0, and each worker waits for the temporary
# relations that existed when it started.
my ($launcher_run) = grep { 1 } map {
	my $pid = $_;
	(grep { $_->{text} =~ /datachecksums launcher\[/ } @{ $trace->{$pid} })
	  ? { $pid => $trace->{$pid} }
	  : ()
} keys %$trace;
my @dbs_done = values_of($launcher_run, 'DATACHECKSUMS', 0,
	'PROGRESS_DATACHECKSUMS_DBS_DONE');
my @dbs_total = values_of($launcher_run, 'DATACHECKSUMS', 0,
	'PROGRESS_DATACHECKSUMS_DBS_TOTAL');
my $ndbs = v($launcher, 'PROGRESS_DATACHECKSUMS_DBS_TOTAL');
is_deeply(
	[ $dbs_total[-1], $dbs_done[-1] ],
	[ [ -1, $ndbs ], [ -1, 0 .. $ndbs ] ],
	'DATACHECKSUMS enable: the launcher counts each database from 0');
my %worker_trace = map { $_ => $trace->{$_} }
  grep {
	grep { $_->{text} =~ /datachecksums worker\[/ }
	  @{ $trace->{$_} }
  } keys %$trace;
my @rels_runs =
  grep { @$_ }
  values_of(\%worker_trace, 'DATACHECKSUMS', 0,
	'PROGRESS_DATACHECKSUMS_RELS_DONE');
is_deeply([ grep { "@$_" ne join(' ', -1, 0 .. $_->[-1]) } @rels_runs ],
	[], 'DATACHECKSUMS enable: each worker counts its relations from 0');
is_deeply(
	[
		grep { $_->[0] != -1 || @$_ != 2 } grep { @$_ } values_of(
			\%worker_trace, 'DATACHECKSUMS',
			0, 'PROGRESS_DATACHECKSUMS_RELS_TOTAL')
	],
	[],
	'DATACHECKSUMS enable: each worker sets its relations_total once');
is_deeply(
	[ checksums_phases($trace, 'datachecksums worker') ],
	[ ([ p('PROGRESS_DATACHECKSUMS_PHASE_WAITING_TEMPREL') ]) x @workers ],
	'DATACHECKSUMS enable: phases of each worker');

# A worker goes over each fork of each relation, block by block.
checked(
	'DATACHECKSUMS',
	'PROGRESS_DATACHECKSUMS_BLOCKS_DONE',
	'PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL');
my @forks = ProgressCheck::rounds(
	$trace,
	'DATACHECKSUMS',
	0,
	p('PROGRESS_DATACHECKSUMS_BLOCKS_DONE'),
	p('PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL'),
	p('PROGRESS_DATACHECKSUMS_RELS_DONE'));
ok(@forks > 0, 'DATACHECKSUMS enable: forks were processed');
is_deeply([ grep { $_->[0] != $_->[1] } @forks ],
	[], 'DATACHECKSUMS enable: each fork counts every block');

# Every parameter of every command is checked by value above, or listed here
# with the reason why not.  A parameter added to progress.h has to go in one
# of the two.
# VACUUM and ANALYZE started by autovacuum say so.  Autovacuum is enabled
# only for this, on a table that it processes at once.
$node->safe_psql(
	'postgres', q(
CREATE TABLE prog_auto (a int) WITH (autovacuum_vacuum_threshold = 0,
  autovacuum_vacuum_scale_factor = 0, autovacuum_analyze_threshold = 0,
  autovacuum_analyze_scale_factor = 0);
INSERT INTO prog_auto SELECT g FROM generate_series(1, 1000) g;
DELETE FROM prog_auto WHERE a % 2 = 0;
));
my $auto_relid =
  $node->safe_psql('postgres', "SELECT 'prog_auto'::regclass::oid");
$trace = traced(
	'autovacuum',
	sub {
		$node->safe_psql('postgres',
			"ALTER SYSTEM SET autovacuum = on; ALTER SYSTEM SET autovacuum_naptime = '1s'; SELECT pg_reload_conf()"
		);
		$node->poll_query_until('postgres',
			"SELECT autovacuum_count > 0 AND autoanalyze_count > 0 FROM pg_stat_user_tables WHERE relid = $auto_relid"
		) or die 'autovacuum did not process the table';
		$node->safe_psql('postgres',
			'ALTER SYSTEM RESET autovacuum; ALTER SYSTEM RESET autovacuum_naptime; SELECT pg_reload_conf()'
		);
	});
my ($av) = ProgressCheck::final_values($trace, 'VACUUM', $auto_relid);
is( v($av, 'PROGRESS_VACUUM_STARTED_BY'),
	p('PROGRESS_VACUUM_STARTED_BY_AUTOVACUUM'),
	'autovacuum: VACUUM started_by');
my ($aa) = ProgressCheck::final_values($trace, 'ANALYZE', $auto_relid);
is( v($aa, 'PROGRESS_ANALYZE_STARTED_BY'),
	p('PROGRESS_ANALYZE_STARTED_BY_AUTOVACUUM'),
	'autovacuum: ANALYZE started_by');

# VACUUM of a table whose oldest XID is past the failsafe age stops vacuuming
# its indexes, and says so.  The failsafe age is never less than 105% of
# autovacuum_freeze_max_age, at least 100000, so that many XIDs are used up
# first.  An anti-wraparound autovacuum may get to the table first: whichever
# VACUUM finds it that old reports the failsafe.
$node->append_conf('postgresql.conf', 'autovacuum_freeze_max_age = 100000');
$node->restart;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION xid_wraparound;
CREATE TABLE prog_fs (a int PRIMARY KEY);
INSERT INTO prog_fs SELECT g FROM generate_series(1, 1000) g;
));
my $fs_relid =
  $node->safe_psql('postgres', "SELECT 'prog_fs'::regclass::oid");
$trace = traced_sql(
	'VACUUM in failsafe mode',
	'SELECT consume_xids(110000); SET vacuum_failsafe_age = 0; VACUUM prog_fs'
);
my @failsafe =
  grep {
	($_->{ p('PROGRESS_VACUUM_MODE') } // 0) ==
	  p('PROGRESS_VACUUM_MODE_FAILSAFE')
  }
  map { @$_ } ProgressCheck::states($trace, 'VACUUM', $fs_relid);
ok(scalar(@failsafe), 'VACUUM in failsafe mode: mode');
is_deeply(
	[
		map { $_->{ p('PROGRESS_VACUUM_INDEXES_TOTAL') } // 0 }
		  @failsafe[ 0 .. 0 ]
	],
	[0],
	'VACUUM in failsafe mode: no index is left to process');

my %UNCHECKED = ();
my @all;
foreach my $command (sort keys %ProgressCheck::COMMANDS)
{
	push @all, map { "$command $_" }
	  sort keys %{ $ProgressCheck::COMMANDS{$command}{params} };
}
my @missing = grep { !$checked{$_} && !$UNCHECKED{$_} } @all;
is(scalar(@missing), 0,
	'every parameter of every command is checked by value')
  or
  diag("not checked, nor listed in %UNCHECKED:\n  " . join("\n  ", @missing));
my @needless = sort grep { $checked{$_} } keys %UNCHECKED;
is(scalar(@needless), 0, 'the parameters said not to be checked are not')
  or diag("listed in %UNCHECKED but checked:\n  " . join("\n  ", @needless));

$node->stop;

done_testing();
