
# Copyright (c) 2026, PostgreSQL Global Development Group

=pod

=head1 NAME

ProgressCheck - parse and check the progress trace of a PROGRESS_DEBUG build

=head1 SYNOPSIS

  use ProgressCheck;

  my $spec = ProgressCheck::load_spec($progress_h);
  my @problems = ProgressCheck::check_spec($spec);
  my $trace = ProgressCheck::parse_log($log_contents);
  my @violations = ProgressCheck::check_trace($spec, $trace);

=head1 DESCRIPTION

A server compiled with PROGRESS_DEBUG logs every change to a backend's
progress state (see backend_progress.c):

  [pid] ... LOG:  progress start: VACUUM relid=16384
  [pid] ... LOG:  progress update: VACUUM relid=16384 0:1->2 8:0->2
  [pid] ... LOG:  progress end: VACUUM relid=16384

This module turns such a log into a per-backend sequence of events, and
checks it against a description of what each command's parameters mean.
The description names parameters by their macro in commands/progress.h,
and load_spec() reads their numbers from that file, so that a parameter
that is added, removed or renumbered there is noticed by check_spec().

=cut

package ProgressCheck;

use strict;
use warnings FATAL => 'all';

use Carp;

# The number of parameters in a backend's progress state; see
# PGSTAT_NUM_PROGRESS_PARAM.
our $NUM_PARAMS = 20;

# The parameters that index AMs write while they build an index.  An index
# build does not know which command it runs under, so these are written
# whatever command is active, or when none is (e.g. the TOAST index built
# by CREATE TABLE).  Their numbers were chosen so as not to collide with
# those of CLUSTER, now REPACK, which rebuilds indexes; check_spec() makes
# sure that stays true for every command marked 'hosts_index_build'.
our @INDEX_BUILD_PARAMS = qw(
  PROGRESS_CREATEIDX_SUBPHASE
  PROGRESS_CREATEIDX_TUPLES_TOTAL
  PROGRESS_CREATEIDX_TUPLES_DONE
  PROGRESS_SCAN_BLOCKS_TOTAL
  PROGRESS_SCAN_BLOCKS_DONE
);

# What each command's parameters mean.
#
# Kinds:
#   phase      the command's phase; only values of the <macro>_* macros
#              (or 0) are valid
#   monotonic  a counter that never decreases during the command
#   resetting  a counter that never decreases, except back to 0 when a new
#              round starts (a new phase, index, child table, ...)
#   free       anything else: totals, OIDs, enum values
#
# A counter may name the parameter that bounds it, [kind, bound]: while the
# bound is positive, the counter must not exceed it.
#
# 'values' lists the prefixes of the macros that are values of the
# command's parameters rather than parameters.
#
# 'null', if set, is the value that the command's view shows as NULL.  A
# counter may go to it and come back from it at any time.
our %COMMANDS = (
	VACUUM => {
		params => {
			PROGRESS_VACUUM_PHASE => 'phase',
			PROGRESS_VACUUM_TOTAL_HEAP_BLKS => 'free',
			PROGRESS_VACUUM_HEAP_BLKS_SCANNED =>
			  [ 'monotonic', 'PROGRESS_VACUUM_TOTAL_HEAP_BLKS' ],
			PROGRESS_VACUUM_HEAP_BLKS_VACUUMED =>
			  [ 'monotonic', 'PROGRESS_VACUUM_TOTAL_HEAP_BLKS' ],
			PROGRESS_VACUUM_NUM_INDEX_VACUUMS => 'monotonic',
			PROGRESS_VACUUM_MAX_DEAD_TUPLE_BYTES => 'free',
			PROGRESS_VACUUM_DEAD_TUPLE_BYTES => 'resetting',
			PROGRESS_VACUUM_NUM_DEAD_ITEM_IDS => 'resetting',
			PROGRESS_VACUUM_INDEXES_TOTAL => 'free',
			PROGRESS_VACUUM_INDEXES_PROCESSED =>
			  [ 'resetting', 'PROGRESS_VACUUM_INDEXES_TOTAL' ],
			PROGRESS_VACUUM_DELAY_TIME => 'monotonic',
			PROGRESS_VACUUM_MODE => 'free',
			PROGRESS_VACUUM_STARTED_BY => 'free',
		},
		values => [
			'PROGRESS_VACUUM_PHASE_', 'PROGRESS_VACUUM_MODE_',
			'PROGRESS_VACUUM_STARTED_BY_'
		],
	},
	ANALYZE => {
		params => {
			PROGRESS_ANALYZE_PHASE => 'phase',
			PROGRESS_ANALYZE_BLOCKS_TOTAL => 'free',
			PROGRESS_ANALYZE_BLOCKS_DONE =>
			  [ 'resetting', 'PROGRESS_ANALYZE_BLOCKS_TOTAL' ],
			PROGRESS_ANALYZE_EXT_STATS_TOTAL => 'free',
			PROGRESS_ANALYZE_EXT_STATS_COMPUTED =>
			  [ 'resetting', 'PROGRESS_ANALYZE_EXT_STATS_TOTAL' ],
			PROGRESS_ANALYZE_CHILD_TABLES_TOTAL => 'free',
			PROGRESS_ANALYZE_CHILD_TABLES_DONE =>
			  [ 'monotonic', 'PROGRESS_ANALYZE_CHILD_TABLES_TOTAL' ],
			PROGRESS_ANALYZE_CURRENT_CHILD_TABLE_RELID => 'free',
			PROGRESS_ANALYZE_DELAY_TIME => 'monotonic',
			PROGRESS_ANALYZE_STARTED_BY => 'free',
		},
		values =>
		  [ 'PROGRESS_ANALYZE_PHASE_', 'PROGRESS_ANALYZE_STARTED_BY_' ],
	},
	REPACK => {
		params => {
			PROGRESS_REPACK_COMMAND => 'free',
			PROGRESS_REPACK_PHASE => 'phase',
			PROGRESS_REPACK_INDEX_RELID => 'free',
			PROGRESS_REPACK_HEAP_TUPLES_SCANNED => 'monotonic',
			PROGRESS_REPACK_HEAP_TUPLES_INSERTED => 'monotonic',
			PROGRESS_REPACK_HEAP_TUPLES_UPDATED => 'monotonic',
			PROGRESS_REPACK_HEAP_TUPLES_DELETED => 'monotonic',
			PROGRESS_REPACK_TOTAL_HEAP_BLKS => 'free',
			PROGRESS_REPACK_HEAP_BLKS_SCANNED =>
			  [ 'monotonic', 'PROGRESS_REPACK_TOTAL_HEAP_BLKS' ],
			PROGRESS_REPACK_INDEX_REBUILD_COUNT => 'monotonic',
		},
		values => ['PROGRESS_REPACK_PHASE_'],
		hosts_index_build => 1,
	},
	CREATE_INDEX => {
		params => {
			PROGRESS_CREATEIDX_COMMAND => 'free',
			PROGRESS_CREATEIDX_INDEX_OID => 'free',
			PROGRESS_CREATEIDX_ACCESS_METHOD_OID => 'free',
			PROGRESS_CREATEIDX_PHASE => 'phase',
			# its values are defined by each index AM
			PROGRESS_CREATEIDX_SUBPHASE => 'free',
			PROGRESS_CREATEIDX_TUPLES_TOTAL => 'free',
			PROGRESS_CREATEIDX_TUPLES_DONE =>
			  [ 'resetting', 'PROGRESS_CREATEIDX_TUPLES_TOTAL' ],
			PROGRESS_CREATEIDX_PARTITIONS_TOTAL => 'free',
			PROGRESS_CREATEIDX_PARTITIONS_DONE =>
			  [ 'monotonic', 'PROGRESS_CREATEIDX_PARTITIONS_TOTAL' ],
			PROGRESS_WAITFOR_TOTAL => 'free',
			PROGRESS_WAITFOR_DONE =>
			  [ 'resetting', 'PROGRESS_WAITFOR_TOTAL' ],
			PROGRESS_WAITFOR_CURRENT_PID => 'free',
			PROGRESS_SCAN_BLOCKS_TOTAL => 'free',
			PROGRESS_SCAN_BLOCKS_DONE =>
			  [ 'resetting', 'PROGRESS_SCAN_BLOCKS_TOTAL' ],
		},
		values => [
			'PROGRESS_CREATEIDX_PHASE_', 'PROGRESS_CREATEIDX_SUBPHASE_',
			'PROGRESS_CREATEIDX_COMMAND_'
		],
	},
	BASEBACKUP => {
		params => {
			PROGRESS_BASEBACKUP_PHASE => 'phase',
			PROGRESS_BASEBACKUP_BACKUP_TOTAL => 'free',
			PROGRESS_BASEBACKUP_BACKUP_STREAMED =>
			  [ 'monotonic', 'PROGRESS_BASEBACKUP_BACKUP_TOTAL' ],
			PROGRESS_BASEBACKUP_TBLSPC_TOTAL => 'free',
			PROGRESS_BASEBACKUP_TBLSPC_STREAMED =>
			  [ 'monotonic', 'PROGRESS_BASEBACKUP_TBLSPC_TOTAL' ],
			PROGRESS_BASEBACKUP_BACKUP_TYPE => 'free',
		},
		values => [
			'PROGRESS_BASEBACKUP_PHASE_', 'PROGRESS_BASEBACKUP_BACKUP_TYPE_'
		],
	},
	COPY => {
		params => {
			PROGRESS_COPY_BYTES_PROCESSED =>
			  [ 'monotonic', 'PROGRESS_COPY_BYTES_TOTAL' ],
			PROGRESS_COPY_BYTES_TOTAL => 'free',
			PROGRESS_COPY_TUPLES_PROCESSED => 'monotonic',
			PROGRESS_COPY_TUPLES_EXCLUDED => 'monotonic',
			PROGRESS_COPY_COMMAND => 'free',
			PROGRESS_COPY_TYPE => 'free',
			PROGRESS_COPY_TUPLES_SKIPPED => 'monotonic',
		},
		values => [ 'PROGRESS_COPY_COMMAND_', 'PROGRESS_COPY_TYPE_' ],
	},
	DATACHECKSUMS => {
		params => {
			PROGRESS_DATACHECKSUMS_PHASE => 'phase',
			PROGRESS_DATACHECKSUMS_DBS_TOTAL => 'free',
			PROGRESS_DATACHECKSUMS_DBS_DONE =>
			  [ 'monotonic', 'PROGRESS_DATACHECKSUMS_DBS_TOTAL' ],
			PROGRESS_DATACHECKSUMS_RELS_TOTAL => 'free',
			PROGRESS_DATACHECKSUMS_RELS_DONE =>
			  [ 'resetting', 'PROGRESS_DATACHECKSUMS_RELS_TOTAL' ],
			PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL => 'free',
			PROGRESS_DATACHECKSUMS_BLOCKS_DONE =>
			  [ 'resetting', 'PROGRESS_DATACHECKSUMS_BLOCKS_TOTAL' ],
		},
		values => ['PROGRESS_DATACHECKSUMS_PHASE_'],
		# the view shows -1 as NULL: not known yet
		null => -1,
	},);

=pod

=head1 FUNCTIONS

=over

=item load_spec($progress_h)

Read the macros of commands/progress.h and resolve %COMMANDS against them.
Returns a hash: 'macros' (name => number) and, per command, 'kind',
'bound' and 'name' indexed by parameter number, and 'phase_values'.

=cut

sub load_spec
{
	my ($progress_h) = @_;
	my %macros;

	open my $fh, '<', $progress_h or croak "could not open $progress_h: $!";
	while (my $line = <$fh>)
	{
		$macros{$1} = $2 if $line =~ /^#define\s+(PROGRESS_\w+)\s+(\d+)\b/;
	}
	close $fh;

	my %spec = (macros => \%macros);
	foreach my $cmd (keys %COMMANDS)
	{
		my %c = (
			kind => {},
			bound => {},
			name => {},
			phase_values => {},
			null => $COMMANDS{$cmd}{null});

		while (my ($macro, $def) = each %{ $COMMANDS{$cmd}{params} })
		{
			my ($kind, $bound) = ref $def ? @$def : ($def);
			next unless defined $macros{$macro};    # reported by check_spec()
			my $n = $macros{$macro};
			$c{kind}{$n} = $kind;
			$c{name}{$n} = $macro;
			$c{bound}{$n} = $macros{$bound}
			  if defined $bound && defined $macros{$bound};
			if ($kind eq 'phase')
			{
				$c{phase_param} = $n;
				$c{phase_values}{0} = 1;
				$c{phase_values}{ $macros{$_} } = 1
				  foreach grep { index($_, "${macro}_") == 0 } keys %macros;
			}
		}
		$spec{$cmd} = \%c;
	}

	# Index build parameters: checked as CREATE INDEX's in the commands that
	# host an index build, and allowed while no command is active.
	my %build;
	foreach my $macro (@INDEX_BUILD_PARAMS)
	{
		next unless defined $macros{$macro};
		my $def = $COMMANDS{CREATE_INDEX}{params}{$macro};
		my ($kind, $bound) = ref $def ? @$def : ($def);
		$build{ $macros{$macro} } = [ $macro, $kind, $bound ];
	}
	$spec{index_build} = { map { $_ => $build{$_}[0] } keys %build };
	foreach my $cmd (grep { $COMMANDS{$_}{hosts_index_build} } keys %COMMANDS)
	{
		my $c = $spec{$cmd};
		foreach my $n (keys %build)
		{
			next if defined $c->{kind}{$n};    # collision, see check_spec()
			my ($macro, $kind, $bound) = @{ $build{$n} };
			$c->{kind}{$n} = $kind;
			$c->{name}{$n} = $macro;
			$c->{bound}{$n} = $macros{$bound}
			  if defined $bound && defined $macros{$bound};
		}
	}
	return \%spec;
}

=pod

=item check_spec($spec)

Compare %COMMANDS with the macros read from progress.h.  Returns a list of
problems: a macro that is not described here, a described parameter that
progress.h does not define, or two parameters of a command that share a
number.

=cut

sub check_spec
{
	my ($spec) = @_;
	my @problems;
	my %claimed;

	foreach my $cmd (sort keys %COMMANDS)
	{
		my %seen;
		foreach my $macro (sort keys %{ $COMMANDS{$cmd}{params} })
		{
			my $def = $COMMANDS{$cmd}{params}{$macro};
			my (undef, $bound) = ref $def ? @$def : ($def);
			$claimed{$macro} = 1;
			if (!defined $spec->{macros}{$macro})
			{
				push @problems, "$cmd: $macro is not defined in progress.h";
				next;
			}
			push @problems, "$cmd: bound $bound of $macro is not defined"
			  if defined $bound && !defined $spec->{macros}{$bound};
			my $n = $spec->{macros}{$macro};
			push @problems, "$cmd: $macro and $seen{$n} are both parameter $n"
			  if defined $seen{$n};
			$seen{$n} = $macro;
		}
	}

	foreach my $cmd (
		sort grep { $COMMANDS{$_}{hosts_index_build} }
		keys %COMMANDS)
	{
		my %own =
		  map  { $spec->{macros}{$_} => $_ }
		  grep { defined $spec->{macros}{$_} }
		  keys %{ $COMMANDS{$cmd}{params} };
		foreach my $macro (@INDEX_BUILD_PARAMS)
		{
			my $n = $spec->{macros}{$macro};
			push @problems,
			  "$cmd: its parameter $own{$n} is $n, which an index build writes as $macro"
			  if defined $n && defined $own{$n};
		}
	}

	foreach my $macro (sort keys %{ $spec->{macros} })
	{
		next if $claimed{$macro};
		next
		  if grep {
			my $cmd = $_;
			grep { index($macro, $_) == 0 } @{ $COMMANDS{$cmd}{values} }
		  } keys %COMMANDS;
		push @problems,
		  "progress.h defines $macro, which is neither a parameter nor a value of any command";
	}
	return @problems;
}

=pod

=item parse_log($contents)

Return the progress events found in a server log, as a hash of pid =>
array of events.  Each event is a hash with 'event' (start, update or end),
'command', 'relid', 'line' (line number in the log), 'text' and, for
updates, 'changes' (a list of [param, old, new]).  The log must have the
pid in brackets in log_line_prefix, as the test framework's default does.

=cut

sub parse_log
{
	my ($contents) = @_;
	my %trace;
	my $lineno = 0;

	foreach my $line (split /\n/, $contents)
	{
		$lineno++;
		next
		  unless $line =~
		  /\[(\d+)\].*?LOG:\s+progress (start|update|end): (\w+) relid=(\d+)(.*)$/;
		my %ev = (
			pid => $1,
			event => $2,
			command => $3,
			relid => $4,
			line => $lineno,
			text => $line);
		my $rest = $5;
		if ($ev{event} eq 'update')
		{
			my @changes;
			while ($rest =~ /\s(\d+):(-?\d+)->(-?\d+)/g)
			{
				push @changes, [ $1, $2, $3 ];
			}
			$ev{changes} = \@changes;
		}
		push @{ $trace{ $ev{pid} } }, \%ev;
	}
	return \%trace;
}

=pod

=item check_trace($spec, $trace)

Replay each backend's events and return the violations found, as hashes
with 'rule', 'pid', 'command', 'line', 'text' and 'detail'.  The rules:

  mismatch      an update names another command or relation than the start
                that is active
  nested        a command started while a different command was active;
                starting the same command again is how a command resets
                its counters (REINDEX CONCURRENTLY does it for each index,
                with the index's table, which may be a TOAST table)
  continuity    the old value of a change is not the value this backend
                had: the state was changed without being logged.  Values
                are only known from the backend's first start on, since a
                backend's parameters are not zeroed until then.
  foreign       a command wrote a parameter that is not one of its own
  phase         a phase took a value that is not defined for it
  decrease      a monotonic counter decreased
  reset         a resetting counter decreased to a value other than 0
  bound         a counter exceeded the parameter that bounds it

Writes made while no command is active are not violations: nothing reads
the parameters then (see pgstat_bestart_initial()), and index builds make
such writes routinely.  stray_writes() counts them.

=cut

sub check_trace
{
	my ($spec, $trace) = @_;
	my @violations;

	foreach my $pid (sort { $a <=> $b } keys %$trace)
	{
		my $active;                          # command name, or undef
		my $relid;
		my @vals = (undef) x $NUM_PARAMS;    # unknown before the first start

		foreach my $ev (@{ $trace->{$pid} })
		{
			my $cmd = $ev->{command};
			my $flag = sub {
				my ($rule, $detail) = @_;
				push @violations,
				  {
					rule => $rule,
					pid => $pid,
					command => $cmd,
					line => $ev->{line},
					text => $ev->{text},
					detail => $detail
				  };
			};

			if ($ev->{event} eq 'start')
			{
				$flag->(
					'nested',
					"$cmd relid=$ev->{relid} started while $active relid=$relid was active"
				) if defined $active && $cmd ne $active;
				$active = $cmd;
				$relid = $ev->{relid};
				@vals = (0) x $NUM_PARAMS;
				next;
			}

			if (!defined $active || $cmd eq 'INVALID')
			{
				# not a violation, see above; only keep the values known
				$flag->('mismatch', "end of $cmd with no command active")
				  if $ev->{event} eq 'end';
			}
			elsif ($cmd ne $active || $ev->{relid} != $relid)
			{
				$flag->(
					'mismatch',
					"$ev->{event} of $cmd relid=$ev->{relid} while $active relid=$relid is active"
				);
			}

			if ($ev->{event} eq 'end')
			{
				undef $active;
				undef $relid;
				next;
			}

			my $c = $spec->{$cmd};
			foreach my $ch (@{ $ev->{changes} })
			{
				my ($n, $old, $new) = @$ch;
				my $name = $c && $c->{name}{$n} ? $c->{name}{$n} : "param $n";

				$flag->(
					'continuity',
					"$name changed from $old, but it was $vals[$n]"
				) if defined $vals[$n] && $old != $vals[$n];
				$vals[$n] = $new;

				next unless $c && defined $active;
				my $kind = $c->{kind}{$n};
				if (!defined $kind)
				{
					$flag->(
						'foreign', "$cmd wrote parameter $n ($old -> $new)");
					next;
				}
				$flag->('phase', "$name took undefined value $new")
				  if $kind eq 'phase' && !$c->{phase_values}{$new};
				next
				  if defined $c->{null}
				  && ($old == $c->{null} || $new == $c->{null});
				$flag->('decrease', "$name decreased from $old to $new")
				  if $kind eq 'monotonic' && $new < $old;
				$flag->('reset', "$name went from $old to $new, not to 0")
				  if $kind eq 'resetting' && $new < $old && $new != 0;
			}

			# Bounds are checked once the whole update is applied, since an
			# update can move a counter and its bound together.
			next unless $c && defined $active;
			foreach my $n (sort { $a <=> $b } keys %{ $c->{bound} })
			{
				my $b = $c->{bound}{$n};
				next
				  unless grep { $_->[0] == $n || $_->[0] == $b }
				  @{ $ev->{changes} };
				# A total of 0 is not known yet, and -1 means it is not
				# known at all (backup_total without a size estimate).
				$flag->(
					'bound',
					"$c->{name}{$n} is $vals[$n], above $spec->{$cmd}{name}{$b} = $vals[$b]"
				) if $vals[$b] > 0 && $vals[$n] > $vals[$b];
			}
		}
	}
	return @violations;
}

=pod

=item commands_run($trace)

Return a hash of command name => number of times it started.

=cut

sub commands_run
{
	my ($trace) = @_;
	my %count;
	foreach my $events (values %$trace)
	{
		$count{ $_->{command} }++
		  foreach grep { $_->{event} eq 'start' } @$events;
	}
	return %count;
}

=pod

=item stray_writes($trace)

Return a hash of parameter number => number of changes made to it while no
command was active.

=cut

sub stray_writes
{
	my ($trace) = @_;
	my %count;
	foreach my $events (values %$trace)
	{
		my $active = 0;
		foreach my $ev (@$events)
		{
			$active = 1 if $ev->{event} eq 'start';
			$active = 0 if $ev->{event} eq 'end';
			next if $active || $ev->{event} ne 'update';
			$count{ $_->[0] }++ foreach @{ $ev->{changes} };
		}
	}
	return %count;
}

=pod

=item phases_of($trace, $spec, $command, $relid)

Return, for each run of $command on $relid (in log order), the list of
phase values it went through, e.g. ([1, 2, 3, 4, 6]).

=cut

sub phases_of
{
	my ($trace, $spec, $command, $relid) = @_;
	my $phase = $spec->{$command}{phase_param};
	croak "$command has no phase parameter" unless defined $phase;

	my @runs;
	foreach my $pid (sort { $a <=> $b } keys %$trace)
	{
		my $current;
		foreach my $ev (@{ $trace->{$pid} })
		{
			next
			  unless $ev->{command} eq $command
			  && (!defined $relid || $ev->{relid} == $relid);
			if ($ev->{event} eq 'start')
			{
				$current = [];
				push @runs, [ $ev->{line}, $current ];
			}
			elsif ($ev->{event} eq 'update' && $current)
			{
				push @$current, map { $_->[2] }
				  grep { $_->[0] == $phase } @{ $ev->{changes} };
			}
		}
	}
	return map { $_->[1] } sort { $a->[0] <=> $b->[0] } @runs;
}

=pod

=item values_of($trace, $command, $relid, $param)

Return, for each run of $command on $relid, the list of values that
parameter number $param took, in order.  This checks a whole succession,
which a final value can hide: a count that jumps ahead and comes back can
still end at the right number.

=cut

sub values_of
{
	my ($trace, $command, $relid, $param) = @_;
	my @runs;
	foreach my $pid (sort { $a <=> $b } keys %$trace)
	{
		my $current;
		foreach my $ev (@{ $trace->{$pid} })
		{
			next
			  unless $ev->{command} eq $command
			  && (!defined $relid || $ev->{relid} == $relid);
			if ($ev->{event} eq 'start')
			{
				$current = [];
				push @runs, [ $ev->{line}, $current ];
			}
			elsif ($ev->{event} eq 'update' && $current)
			{
				push @$current, map { $_->[2] }
				  grep { $_->[0] == $param } @{ $ev->{changes} };
			}
		}
	}
	return map { $_->[1] } sort { $a->[0] <=> $b->[0] } @runs;
}

=pod

=item final_values($trace, $command, $relid)

Return, for each run of $command on $relid, a hash of parameter number =>
the value it had when the command ended.

=cut

sub final_values
{
	my ($trace, $command, $relid) = @_;
	my @runs;
	foreach my $pid (sort { $a <=> $b } keys %$trace)
	{
		my %vals;
		foreach my $ev (@{ $trace->{$pid} })
		{
			next
			  unless $ev->{command} eq $command
			  && (!defined $relid || $ev->{relid} == $relid);
			%vals = () if $ev->{event} eq 'start';
			$vals{ $_->[0] } = $_->[2] foreach @{ $ev->{changes} || [] };
			push @runs, [ $ev->{line}, {%vals} ] if $ev->{event} eq 'end';
		}
	}
	return map { $_->[1] } sort { $a->[0] <=> $b->[0] } @runs;
}

=pod

=back

=cut

1;
