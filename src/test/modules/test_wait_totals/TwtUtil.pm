# Copyright (c) 2026, PostgreSQL Global Development Group

# Helpers shared by the test_wait_totals TAP tests.
#
# harden($node) re-blesses a PostgreSQL::Test::Cluster so that every
# synchronous psql call has an external timeout (a stuck reader or a lock
# deadlock then fails the test instead of hanging it), and so that
# poll_query_until() bounds each individual attempt, not only the number of
# attempts.  BackgroundPsql sessions have their own timer.

package TwtUtil;

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils ();
use Time::HiRes qw(usleep time);

sub harden
{
	my ($node) = @_;

	bless $node, 'TwtUtil::Node';
	return $node;
}

package TwtUtil::Node;

use strict;
use warnings FATAL => 'all';

use parent -norequire, 'PostgreSQL::Test::Cluster';
use Time::HiRes qw(usleep time);

sub psql
{
	my ($self, $dbname, $sql, %params) = @_;

	$params{timeout} = $PostgreSQL::Test::Utils::timeout_default
	  unless defined $params{timeout};
	return $self->SUPER::psql($dbname, $sql, %params);
}

sub poll_query_until
{
	my ($self, $dbname, $query, $expected) = @_;

	$expected = 't' unless defined $expected;
	my $deadline = time() + $PostgreSQL::Test::Utils::timeout_default;
	my $last = '';

	while (time() < $deadline)
	{
		my ($stdout, $stderr, $timed_out) = ('', '', 0);

		# One attempt, itself bounded.
		$self->SUPER::psql($dbname, $query,
			stdout => \$stdout, stderr => \$stderr,
			timeout => 10, timed_out => \$timed_out,
			on_error_stop => 0);
		chomp($stdout);
		chomp($stderr);
		$last = "stdout=$stdout stderr=$stderr timed_out=$timed_out";
		# A timed-out attempt is fatal: killing psql does not cancel its
		# backend, which may be stuck, so retrying would only pile up
		# sessions behind it.  Dying tears the test (and the node) down now.
		die "poll_query_until: an attempt timed out on: $query" if $timed_out;
		return 1 if $stdout eq $expected && $stderr eq '';
		usleep(100_000);
	}

	Test::More::diag("poll_query_until timed out: $query\nexpected: $expected\nlast: $last");
	return 0;
}

1;
