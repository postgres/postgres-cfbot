# Copyright (c) 2026, PostgreSQL Global Development Group

=pod

=head1 NAME

DocPhases - cross-check the documented phases against the ones a command
really reports

=head1 SYNOPSIS

  use DocPhases;

  my $doc = DocPhases::load($share_dir);
  my @problems = DocPhases::check_against_trace($doc, 'VACUUM', \@phases_seen);

=head1 DESCRIPTION

A progress phase exists in three places in the tree, and nothing checks
that the three agree:

=over

=item * F<src/include/commands/progress.h> defines the value
(C<PROGRESS_VACUUM_PHASE_SCAN_HEAP> is 1).

=item * F<src/backend/catalog/system_views.sql> turns the value into the
text the user sees (C<WHEN 1 THEN 'scanning heap'>).

=item * F<doc/src/sgml/monitoring.sgml> lists the phases in a table.

=back

This module reads the last two and compares them with what a command
actually reported, so that a phase the view does not name, or the
documentation does not list, becomes visible.  The row order of the table
is not checked: it is not meant as a guarantee of the order of execution.

Nothing here is hardcoded: adding a phase in the three places keeps the
check quiet, adding it in two of them does not.

=cut

package DocPhases;

use strict;
use warnings FATAL => 'all';

use Carp;

# view name -> the phase table in monitoring.sgml
our %PHASE_TABLE = (
	pg_stat_progress_vacuum => 'vacuum-phases',
	pg_stat_progress_analyze => 'analyze-phases',
	pg_stat_progress_cluster => 'cluster-phases',
	pg_stat_progress_repack => 'repack-phases',
	pg_stat_progress_create_index => 'create-index-phases',
	pg_stat_progress_basebackup => 'basebackup-phases',
);

# command name as PROGRESS_DEBUG logs it -> view name
our %COMMAND_VIEW = (
	VACUUM => 'pg_stat_progress_vacuum',
	ANALYZE => 'pg_stat_progress_analyze',
	CLUSTER => 'pg_stat_progress_cluster',
	REPACK => 'pg_stat_progress_repack',
	# progress.h calls it CREATE_INDEX, which is how PROGRESS_DEBUG logs it
	CREATE_INDEX => 'pg_stat_progress_create_index',
	BASEBACKUP => 'pg_stat_progress_basebackup',
);

=pod

=head2 load($system_views_sql, $monitoring_sgml)

Read the phase texts from system_views.sql and the phases documented in
monitoring.sgml.  Returns a hashref keyed by view name, each holding

  value_to_text   { 0 => 'initializing', 1 => 'scanning heap', ... }
  documented      { 'initializing' => 1, 'scanning heap' => 1, ... }

The two paths come from the Makefile, the same way the module that reads
progress.h gets its own.

=cut

sub load
{
	my ($system_views_sql, $monitoring_sgml) = @_;
	my %out;

	my $views = _slurp($system_views_sql);
	my $sgml = _slurp($monitoring_sgml);

	for my $view (keys %PHASE_TABLE)
	{
		# the CASE that maps the phase parameter to its text
		next unless $views =~ /CREATE VIEW \Q$view\E AS(.*?);\n/s;
		my $body = $1;
		# The phase CASE, and only it: some views have another CASE just
		# before it (create_index maps param1 to a command name), so the
		# match must not run from one CASE into the next one's END.  The
		# keyword case varies too ("END as phase").
		next
		  unless $body =~
		  /CASE \s+ S\.param\d+ ((?: (?! \bEND\s+AS\b ) . )*?) \s+ END \s+ AS \s+ phase/isx;
		my $case = $1;

		my %v2t;
		while ($case =~ /WHEN\s+(\d+)\s+THEN\s+'([^']*)'/g)
		{
			$v2t{$1} = $2;
		}
		next unless %v2t;

		my $id = $PHASE_TABLE{$view};
		my %documented;
		if ($sgml =~ /<table id="\Q$id\E">(.*?)<\/table>/s)
		{
			my $table = $1;
			while ($table =~ /<entry><literal>([^<]*)<\/literal><\/entry>/g)
			{
				$documented{$1} = 1;
			}
		}

		$out{$view} =
		  { value_to_text => \%v2t, documented => \%documented };
	}

	return \%out;
}

=pod

=head2 check_against_trace($doc, $command, $phases)

$phases is the sequence of phase values a single command run reported, as
ProgressCheck::phases_of() returns it.  Returns a list of problems, each a
hashref with 'kind' and 'detail'.  The only kind is 'undocumented': a value
was reported that system_views.sql does not map to a text, or whose text
the documentation table does not list.  Each such value is reported once.

=cut

sub check_against_trace
{
	my ($doc, $command, $phases) = @_;
	my @problems;

	my $view = $COMMAND_VIEW{$command} or return ();
	my $d = $doc->{$view} or return ();

	my %reported;
	for my $val (@$phases)
	{
		next if $reported{$val}++;

		my $text = $d->{value_to_text}{$val};
		if (!defined $text)
		{
			push @problems,
			  {
				kind => 'undocumented',
				detail => "$command reported phase value $val, which "
				  . "system_views.sql does not map to any text"
			  };
		}
		elsif (!$d->{documented}{$text})
		{
			push @problems,
			  {
				kind => 'undocumented',
				detail => "$command reported phase \"$text\", which is not "
				  . "listed in the documentation table"
			  };
		}
	}

	return @problems;
}

sub _slurp
{
	my ($path) = @_;
	open my $fh, '<', $path or croak "could not open $path: $!";
	local $/;
	my $c = <$fh>;
	close $fh;
	return $c;
}

1;
