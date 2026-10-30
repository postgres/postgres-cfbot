#!/usr/bin/perl
#----------------------------------------------------------------------
#
# Generate a TraceLogging probes.h for ETW builds from probes.d and
# probes_etw.txt.
#
# Reads the probe argument types from probes.d and the ETW keyword, level,
# and field names from probes_etw.txt, and emits the same TRACE_POSTGRESQL_*
# macros as the dtrace and dummy backends.  Dies if the two files disagree on
# the probe set or on any argument's type, or if probes.d uses a type with no
# mapping below.
#
# With --check, only the checks are done and nothing is written.  Builds
# without ETW run it that way, so that probes_etw.txt is kept in step with
# probes.d on every platform.
#
# Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/backend/utils/Gen_etw_probes.pl
#
#----------------------------------------------------------------------

use strict;
use warnings FATAL => 'all';
use Getopt::Long;

# probes.d type -> [ TraceLogging macro, cast applied to the argument value ].
# The cast is '' for none, 'STRING' for strings (see below), or a C cast token
# to prepend, used to widen 'long' and the pointer-sized size_t and ssize_t to
# a fixed 64-bit field.
my %type_map = (
	'LocalTransactionId' => [ 'TraceLoggingUInt32', '' ],
	'BlockNumber' => [ 'TraceLoggingUInt32', '' ],
	'Oid' => [ 'TraceLoggingUInt32', '' ],
	'unsigned int' => [ 'TraceLoggingUInt32', '' ],
	'LWLockMode' => [ 'TraceLoggingInt32', '' ],
	'LOCKMODE' => [ 'TraceLoggingInt32', '' ],
	'ForkNumber' => [ 'TraceLoggingInt32', '' ],
	'int' => [ 'TraceLoggingInt32', '' ],
	'bool' => [ 'TraceLoggingBoolean', '' ],
	'unsigned char' => [ 'TraceLoggingUInt8', '' ],
	'long' => [ 'TraceLoggingInt64', '(LONGLONG)' ],
	'ssize_t' => [ 'TraceLoggingInt64', '(LONGLONG)' ],
	'size_t' => [ 'TraceLoggingUInt64', '(ULONGLONG)' ],
	'const char *' => [ 'TraceLoggingCountedUtf8String', 'STRING' ],);

my %keyword_token = map { $_ => "PG_KW_$_" }
  qw(QUERY TRANSACTION LOCK BUFFER STORAGE WAL CHECKPOINT SORT);

my %level_token = (
	INFO => 'WINEVENT_LEVEL_INFO',
	VERBOSE => 'WINEVENT_LEVEL_VERBOSE',);

my $check_only = 0;

GetOptions('check' => \$check_only)
  or die "usage: $0 [--check] probes.d probes_etw.txt\n";

my ($probes_d, $probes_etw) = @ARGV;
defined $probes_d && defined $probes_etw
  or die "usage: $0 [--check] probes.d probes_etw.txt\n";

# Read probes.d into a map from probe name to its list of argument types.
my @order;    # probes.d order, used for the output
my %args;
open(my $pd, '<', $probes_d) or die "could not open \"$probes_d\": $!\n";
while (my $line = <$pd>)
{
	next unless $line =~ /^\s*probe\s+([^(]+)\(([^)]*)\)\s*;/;
	my ($name, $arglist) = ($1, $2);
	$name =~ s/\s+//g;
	$arglist =~ s/^\s+|\s+$//g;
	my @types;
	if (length $arglist)
	{
		for my $t (split /\s*,\s*/, $arglist)
		{
			$t =~ s/^\s+|\s+$//g;
			$t =~ s/\s+/ /g;
			$t =~ s/\s*\*$/ */;
			push @types, $t;
		}
	}
	push @order, $name;
	$args{$name} = \@types;
}
close $pd;

# Read probes_etw.txt into a map from probe name to its keyword, level,
# argument types and field names.
my %policy;
open(my $pf, '<', $probes_etw) or die "could not open \"$probes_etw\": $!\n";
while (my $line = <$pf>)
{
	$line =~ s/#.*$//;
	next unless $line =~ /\S/;
	$line =~ s/^\s+|\s+$//g;
	my ($name, $kw, $lvl, $arglist) = split ' ', $line, 4;
	defined $kw && defined $lvl
	  or die "$probes_etw: probe \"$name\" is missing keyword/level\n";
	exists $keyword_token{$kw}
	  or die "$probes_etw: probe \"$name\" has unknown keyword \"$kw\"\n";
	exists $level_token{$lvl}
	  or die "$probes_etw: probe \"$name\" has unknown level \"$lvl\"\n";

	# Each argument is "type field-name", as in a C prototype.
	my (@types, @fields);
	if (defined $arglist)
	{
		for my $decl (split /\s*,\s*/, $arglist)
		{
			$decl =~ /^(.*?)\s*(\w+)$/
			  or die "$probes_etw: probe \"$name\": bad argument \"$decl\"\n";
			my ($t, $f) = ($1, $2);
			$t =~ s/\s+/ /g;
			$t =~ s/\s*\*$/ */;
			push @types, $t;
			push @fields, $f;
		}
	}
	$policy{$name} = {
		keyword => $kw,
		level => $lvl,
		types => \@types,
		fields => \@fields
	};
}
close $pf;

# Check that the two files agree.
for my $name (@order)
{
	exists $policy{$name}
	  or die "probe \"$name\" is in $probes_d but missing from $probes_etw\n";
	my $nargs = scalar @{ $args{$name} };
	my $nfields = scalar @{ $policy{$name}{fields} };
	$nargs == $nfields
	  or die "probe \"$name\": $probes_d has $nargs argument(s) but "
	  . "$probes_etw names $nfields field(s)\n";
	for my $i (0 .. $nargs - 1)
	{
		my $t = $args{$name}[$i];
		my $pt = $policy{$name}{types}[$i];
		$t eq $pt
		  or die "probe \"$name\": argument "
		  . ($i + 1)
		  . " is \"$t\" in $probes_d but \"$pt\" in $probes_etw\n";
		exists $type_map{$t}
		  or die
		  "$probes_d: probe \"$name\": no ETW type mapping for \"$t\"\n";
	}
}
for my $name (sort keys %policy)
{
	exists $args{$name}
	  or die "probe \"$name\" is in $probes_etw but missing from $probes_d\n";
}

exit 0 if $check_only;

# Write the header.
print <<'EOH';
/*
 * This file is generated by Gen_etw_probes.pl from probes.d and
 * probes_etw.txt.  Do not edit by hand.
 *
 * It maps the TRACE_POSTGRESQL_* probe macros to ETW (Event Tracing for
 * Windows) TraceLogging events.  The macro names are identical to those of the
 * DTrace and dummy backends, so probe call sites are unchanged.
 */

#include "utils/pg_trace_etw.h"

EOH

for my $name (@order)
{
	my @types = @{ $args{$name} };
	my $pol = $policy{$name};
	my @fields = @{ $pol->{fields} };
	my $kw = $keyword_token{ $pol->{keyword} };
	my $lvl = $level_token{ $pol->{level} };

	# The event name is the probe name with each double underscore collapsed
	# to one, i.e. the DTrace name with underscores in place of hyphens.
	(my $event = $name) =~ s/__/_/g;

	# The macro name is TRACE_POSTGRESQL_ plus the upper-cased event name.
	(my $macro = $event) =~ tr/a-z/A-Z/;
	$macro = "TRACE_POSTGRESQL_$macro";

	# Macro parameters are positional (arg1, arg2, ...), as in the other
	# backends; the field names come from probes_etw.txt.
	my @params = map { "arg$_" } 1 .. @types;
	my $paramlist = join(', ', @params);

	# String arguments become counted UTF-8 strings, clipped by
	# pg_trace_etw_strlen().  The pointer is used twice, so it is evaluated
	# once into a local, inside a check that the event is enabled.
	my $has_string = scalar grep { $type_map{$_}[1] eq 'STRING' } @types;
	my $ind = $has_string ? "\t\t\t" : "\t";

	my @values;
	my @locals;
	for my $i (0 .. $#types)
	{
		my ($tlmacro, $cast) = @{ $type_map{ $types[$i] } };
		my $arg = $params[$i];
		my $field = $fields[$i];
		my $value;
		if ($cast eq 'STRING')
		{
			my $local = "pg_etw_$arg";
			push @locals, "const char *$local = ($arg);";
			$value = "$local ? $local : \"\", pg_trace_etw_strlen($local)";
		}
		elsif (length $cast)
		{
			$value = "$cast($arg)";
		}
		else
		{
			$value = "($arg)";
		}
		push @values, "$tlmacro($value, \"$field\")";
	}

	print "/* probe $name(" . join(', ', @types) . "); */\n";
	print "#define $macro($paramlist) \\\n";
	if ($has_string)
	{
		print "\tdo { \\\n";
		print "\t\tif (${macro}_ENABLED()) \\\n";
		print "\t\t{ \\\n";
		print "\t\t\t$_ \\\n" for @locals;
	}
	print "${ind}TraceLoggingWrite(pg_etw_provider, \"$event\", \\\n";
	print "${ind}\tTraceLoggingKeyword($kw), \\\n";
	print "${ind}\tTraceLoggingLevel($lvl)";
	print ", \\\n${ind}\t$_" for @values;
	print ")";
	if ($has_string)
	{
		print "; \\\n";
		print "\t\t} \\\n";
		print "\t} while (0)";
	}
	print "\n";

	print "#define ${macro}_ENABLED() \\\n";
	print "\tTraceLoggingProviderEnabled(pg_etw_provider, $lvl, $kw)\n\n";
}
