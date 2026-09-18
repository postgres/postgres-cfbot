# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: pg_wait_event_trace_by_statement() under a
# realistic client load.
#
# The earlier by-statement tests (the regress script, t/014) run one
# statement at a time from a hand-written session.  A real client sends
# the same statement shapes over and over, over three different protocols
# (simple, extended, extended with prepared statements), from several
# connections at once, and a heavy lock on a hot table makes all of them
# wait in the places where a wait is not yet attributable to a statement:
# plan cache locks in Bind, relation locks in parse analysis.  That is the
# convoy case seen in the field, where waits taken before a statement's
# start marker were reported in '<idle>'.
#
# For each of pgbench's three protocol modes this test runs 4 clients of a
# one-statement read-only script against a small table, with
# pg_wait_event_tracing.capture = trace (passed through PGOPTIONS, so only
# the clients are traced).  Once every client has run a few statements --
# so that in prepared mode the statements are already cached and the lock
# is taken in Bind -- a second, untraced session takes ACCESS EXCLUSIVE on
# the table, waits (polling pg_locks) until at least one client is blocked,
# holds the lock for a short while so that the wait is long enough to be
# recorded, and commits.
#
# The clients' rings are read after pgbench has finished.  The ring of an
# exited backend is kept as an orphan and stays readable by procnumber, so
# there is no need to keep the clients alive; each client's procnumber is
# looked up (by pid) while it is still connected.  A ring that is complete
# and final also lets the invariant below be exact.  Per client:
#
#   (1) sum(calls) over the function equals the number of non-marker
#       records of the ring: no wait lost, none counted twice.
#   (2) no wait of a type other than 'Client' is in '<idle>'.
#   (3) over all clients of the mode, at least one Lock:relation wait was
#       recorded, and every one of them is in a statement bucket (not
#       '<idle>', '<no-statement>' or '<unattributed>'), phase 'pre' or
#       'statement'.  Which phase they land in is reported with note().
#   (4) every statement bucket has a non-zero query_id.
#
# Nothing here depends on how many waits a statement records or on timing,
# apart from the wait for the pgbench clients to get going and to block,
# which is polled.

use strict;
use warnings FATAL => 'all';

use IPC::Run ();
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf(
	'postgresql.conf', q(
shared_preload_libraries = 'pg_wait_event_tracing'
debug_parallel_query = off
compute_query_id = on
max_connections = 30
# Large enough that no client's ring wraps during a run, so that nothing
# is '<unattributed>' for that reason.
pg_wait_event_tracing.trace_ring_size = 8192
));
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE TABLE pwet_pgb_t (id int PRIMARY KEY, payload text);
INSERT INTO pwet_pgb_t SELECT g, 'x' FROM generate_series(1, 10) g;
));

my $script = $node->basedir . '/pwet_pgb.sql';
append_to_file($script,
	"\\set id random(1, 10)\n"
	  . "SELECT payload FROM pwet_pgb_t WHERE id = :id;\n");

my $clients = 4;

# The session that blocks the clients.  Not traced.
my $locker = $node->background_psql('postgres');

# Procnumbers of the pgbench backends and the pids they belong to.
sub client_backends
{
	my $out = $node->safe_psql(
		'postgres', q(
		SELECT id || '|' || pg_stat_get_backend_pid(id)
		FROM pg_stat_get_backend_idset() AS id
		WHERE pg_stat_get_backend_pid(id) IN
		      (SELECT pid FROM pg_stat_activity
		       WHERE application_name = 'pwet_pgbench')
		ORDER BY 1;
	));
	return map { [ split /\|/ ] } split /\n/, $out;
}

foreach my $mode (qw(simple extended prepared))
{
	# -R keeps the rate (and with it the ring fill) low and predictable.
	# -T is only an upper bound for how long the clients run; the lock
	# cycle below normally takes a fraction of it.
	my @cmd = (
		'pgbench', '-n', '-M', $mode, '-c', $clients, '-j', 1,
		'-R', 200, '-T', 6, '-f', $script, '-d', 'postgres',
		'-h', $node->host, '-p', $node->port);
	my ($pgb_out, $pgb_err) = ('', '');
	my $pgb;
	{
		local $ENV{PGOPTIONS} = '-c pg_wait_event_tracing.capture=trace';
		# how the clients are told apart from the test's own sessions
		local $ENV{PGAPPNAME} = 'pwet_pgbench';
		$pgb = IPC::Run::start(\@cmd, '>' => \$pgb_out, '2>' => \$pgb_err,
			IPC::Run::timeout($PostgreSQL::Test::Utils::timeout_default));
	}

	# All clients connected, and each has completed a few statements
	# (ExecEnd markers in its own ring).
	my @be;
	my $ready = 0;
	for (1 .. $PostgreSQL::Test::Utils::timeout_default * 10)
	{
		@be = client_backends();
		if (@be == $clients)
		{
			my $n = scalar grep {
				$node->safe_psql('postgres',
					"SELECT count(*) >= 20 FROM pg_get_wait_event_trace($_->[0]) "
					  . "WHERE wait_event = 'ExecEnd';") eq 't'
			} @be;
			if ($n == $clients) { $ready = 1; last; }
		}
		select(undef, undef, undef, 0.1);
	}
	ok($ready, "$mode: $clients pgbench clients connected and running");

	my $pid_list = join(',', map { $_->[1] } @be);

	# Take the lock.  This waits for statements in flight, and the
	# clients queue behind it.
	$locker->query_safe(
		"BEGIN;\nLOCK TABLE pwet_pgb_t IN ACCESS EXCLUSIVE MODE;");
	$node->poll_query_until(
		'postgres', qq(
		SELECT EXISTS (SELECT 1 FROM pg_locks
		               WHERE NOT granted AND locktype = 'relation'
		                 AND relation = 'pwet_pgb_t'::regclass
		                 AND pid IN ($pid_list));
	)) or die "no pgbench client ever blocked on the table lock";
	# The only fixed delay: make the wait long enough to be recorded.
	select(undef, undef, undef, 0.2);
	$locker->query_safe('COMMIT;');

	$pgb->finish;
	is($pgb->result(0), 0, "$mode: pgbench finished successfully")
	  or diag($pgb_err);

	$node->poll_query_until('postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity "
		  . "WHERE application_name = 'pwet_pgbench');")
	  or die "pgbench backends did not exit";

	my ($lock_total, %lock_phase) = (0);
	foreach my $c (@be)
	{
		my ($pn, $pid) = @$c;
		my $name = "$mode backend $pid";

		# The ring must still be this client's (an orphan).
		is( $node->safe_psql(
				'postgres',
				"SELECT count(DISTINCT owner_pid) || ',' || min(owner_pid) "
				  . "FROM pg_get_wait_event_trace($pn);"),
			"1,$pid",
			"$name: ring is the client's, read post-mortem");

		# (1) every non-marker record counted exactly once
		is( $node->safe_psql(
				'postgres', qq(
				SELECT (SELECT sum(calls)
				        FROM pg_wait_event_trace_by_statement($pn))
				     = (SELECT count(*) FROM pg_get_wait_event_trace($pn)
				        WHERE wait_event_type <> 'Query');
			)),
			't',
			"$name: sum(calls) equals the number of non-marker records");

		# (2) nothing but Client waits in <idle>
		is( $node->safe_psql(
				'postgres', qq(
				SELECT coalesce(string_agg(DISTINCT wait_event_type || ':'
				                           || wait_event, ','), '')
				FROM pg_wait_event_trace_by_statement($pn)
				WHERE bucket = '<idle>' AND wait_event_type <> 'Client';
			)),
			'',
			"$name: no non-Client wait in <idle>");

		# (4) statement buckets carry a query_id
		is( $node->safe_psql(
				'postgres', qq(
				SELECT count(*)
				FROM pg_wait_event_trace_by_statement($pn)
				WHERE statement_seq IS NOT NULL
				  AND (query_id IS NULL OR query_id = 0);
			)),
			'0',
			"$name: every statement bucket has a non-zero query_id");

		# (3) collect Lock:relation waits: bucket kind and phase
		my $rows = $node->safe_psql(
			'postgres', qq(
			SELECT CASE WHEN statement_seq IS NOT NULL THEN 'statement'
			            ELSE bucket END || '|' || coalesce(phase, 'NULL')
			       || '|' || calls
			FROM pg_wait_event_trace_by_statement($pn)
			WHERE wait_event_type = 'Lock' AND wait_event = 'relation';
		));
		foreach my $r (split /\n/, $rows)
		{
			my ($kind, $phase, $calls) = split /\|/, $r;
			$lock_total += $calls;
			$lock_phase{"$kind/$phase"} += $calls;
		}
	}

	# (3)
	cmp_ok($lock_total, '>=', 1,
		"$mode: at least one Lock:relation wait was recorded");
	my @bad = grep { !/^statement\/(pre|statement)$/ } sort keys %lock_phase;
	is(scalar @bad, 0,
		"$mode: every Lock:relation wait is in a statement bucket, "
		  . "phase pre or statement")
	  or diag("Lock:relation waits by bucket/phase: "
		  . join(', ', map { "$_=$lock_phase{$_}" } sort keys %lock_phase));
	note("$mode: Lock:relation waits by bucket/phase: "
		  . join(', ', map { "$_=$lock_phase{$_}" } sort keys %lock_phase));
}

$locker->quit;
$node->stop;
done_testing();
