# Copyright (c) 2026, PostgreSQL Global Development Group

# Check the progress phases a command really reports against the phases the
# documentation lists for it.
#
# A phase lives in three places that nothing keeps in sync: its value in
# progress.h, its text in system_views.sql, and its row in the table in
# monitoring.sgml.  This test runs the commands and compares.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin . '/..';

use ProgressCheck;
use DocPhases;

my $progress_h = $ENV{PROGRESS_H}
  or plan skip_all => 'PROGRESS_H is not set';
plan skip_all => 'not compiled with PROGRESS_DEBUG'
  unless ProgressCheck::compiled_with_progress_debug($progress_h);

my $system_views = $ENV{SYSTEM_VIEWS_SQL}
  or plan skip_all => 'SYSTEM_VIEWS_SQL is not set';
my $monitoring = $ENV{MONITORING_SGML}
  or plan skip_all => 'MONITORING_SGML is not set';

my $node = PostgreSQL::Test::Cluster->new('doc_phases');
$node->init;
# several rounds of index vacuuming, so that VACUUM reports each of its
# phases more than once
$node->append_conf('postgresql.conf', 'maintenance_work_mem = 1024');
# REPACK (CONCURRENTLY) refuses to run below "replica"
$node->append_conf('postgresql.conf', 'wal_level = replica');
$node->start;

$node->safe_psql('postgres',
	q{CREATE TABLE phases_tab (a int primary key, b text)});
$node->safe_psql('postgres',
	q{INSERT INTO phases_tab SELECT g, repeat('y', 40) FROM generate_series(1, 200000) g});
$node->safe_psql('postgres', q{CREATE INDEX phases_b ON phases_tab (b)});
$node->safe_psql('postgres', q{DELETE FROM phases_tab});

my $doc = DocPhases::load($system_views, $monitoring);
ok(keys %$doc, 'found phase tables in the documentation');

my $spec = ProgressCheck::load_spec($progress_h);

my @commands = (
	[ 'VACUUM' => 'VACUUM phases_tab' ],
	[ 'ANALYZE' => 'ANALYZE phases_tab' ],
	[ 'CLUSTER' => 'CLUSTER phases_tab USING phases_tab_pkey' ],
	[ 'CREATE INDEX CONCURRENTLY' =>
		  'CREATE INDEX CONCURRENTLY phases_c ON phases_tab (a)' ],
	[ 'REPACK (CONCURRENTLY)' => 'REPACK (CONCURRENTLY) phases_tab' ],
);

for my $c (@commands)
{
	my ($name, $sql) = @$c;

	my $offset = -s $node->logfile;
	$node->safe_psql('postgres', $sql);

	my $contents = slurp_file($node->logfile, $offset);
	my $trace = ProgressCheck::parse_log($contents);

	my %ran = ProgressCheck::commands_run($trace);
	for my $command (sort keys %ran)
	{
		next unless defined $spec->{$command}{phase_param};

		for my $phases (ProgressCheck::phases_of($trace, $spec, $command, undef))
		{
			next unless @$phases;

			# A value the view or the documentation does not know about is a
			# plain bug: the three places have drifted apart.
			my @undocumented =
			  DocPhases::check_against_trace($doc, $command, $phases);
			is(scalar @undocumented, 0,
				"$name: every phase $command reports is documented")
			  or diag(join("\n", map { $_->{detail} } @undocumented));
		}
	}
}

done_testing();
