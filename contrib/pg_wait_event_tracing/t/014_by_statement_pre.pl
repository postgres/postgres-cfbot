# Copyright (c) 2026, PostgreSQL Global Development Group

# pg_wait_event_tracing: pg_wait_event_trace_by_statement() attribution of
# waits that happen after a request has arrived but before its statement
# start marker is written -- the 'pre' phase -- and of requests that fail
# before any statement starts.
#
# Two sessions: A (not traced) holds an ACCESS EXCLUSIVE lock on a table,
# B (traced) runs into it.  A deterministic .sql script cannot do this,
# because it needs a second session holding a lock across B's request.
# The waiter is detected by polling pg_locks, never by a fixed sleep; the
# only fixed delay is the time A keeps holding the lock once B is known
# to wait, which just makes the wait long enough to be recorded.
#
#   (1) extended protocol, named prepared statement: the relation lock is
#       taken in Bind (plan cache), before ExecStart.  The wait must be in
#       the statement bucket opened by that ExecStart, phase 'pre'.
#   (2) simple protocol: the lock is taken in parse analysis, before
#       QueryStart.  Same expectation, bucket opened by the QueryStart.
#   (3) lock_timeout expires during Bind of a prepared statement: no
#       statement ever starts; the wait belongs to '<no-statement>'.
#   Finally, on the then idle session B, the function's calls add up to
#   exactly the number of non-marker records of its ring.

use strict;
use warnings FATAL => 'all';

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
));
$node->start;
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION pg_wait_event_tracing;
CREATE TABLE pwet_pre_t (id int PRIMARY KEY, payload text);
INSERT INTO pwet_pre_t SELECT g, 'x' FROM generate_series(1, 10) g;
));

my $psql_a = $node->background_psql('postgres');
my $psql_b = $node->background_psql('postgres', on_error_stop => 0);

$psql_b->query_safe('SET pg_wait_event_tracing.capture = trace;');
my $b_pid = $psql_b->query_safe('SELECT pg_backend_pid();');
# B's own lookup: a cross-backend read of its timing rows could lag behind
# (its last wait is flushed only at its next wait_start).
my $b_pn = $psql_b->query_safe(
	'SELECT id FROM pg_stat_get_backend_idset() AS id '
	  . 'WHERE pg_stat_get_backend_pid(id) = pg_backend_pid();');
chomp($b_pid);
chomp($b_pn);

# Prepare before any lock is held, so the Parse itself does not block.
$psql_b->query_safe(
	"SELECT count(*) AS n FROM pwet_pre_t \\parse pwet_pre_s\n");

sub ring_mark
{
	return $node->safe_psql('postgres',
		"SELECT coalesce(max(seq), -1) FROM pg_get_wait_event_trace($b_pn);"
	);
}

sub wait_for_b_blocked
{
	$node->poll_query_until(
		'postgres', qq(
		SELECT EXISTS (SELECT 1 FROM pg_locks
		               WHERE NOT granted AND locktype = 'relation'
		                 AND relation = 'pwet_pre_t'::regclass
		                 AND pid = $b_pid);
	)) or die "backend $b_pid never blocked on the table lock";
}

# Lock:relation rows after $mark: "<phase>|<opening marker>|<query_id ok>"
sub lock_rows
{
	my ($mark) = @_;
	return $node->safe_psql(
		'postgres', qq(
		SELECT string_agg(coalesce(b.phase, 'NULL') || '|' || m.wait_event
		                  || '|' || (b.query_id = m.query_id AND b.query_id <> 0),
		                  ',' ORDER BY b.statement_seq)
		FROM pg_wait_event_trace_by_statement($b_pn) b
		     JOIN pg_get_wait_event_trace($b_pn) m ON m.seq = b.statement_seq
		WHERE b.wait_event_type = 'Lock' AND b.wait_event = 'relation'
		  AND b.statement_seq > $mark;
	));
}

sub lock_calls_in
{
	my ($bucket) = @_;
	return $node->safe_psql(
		'postgres', qq(
		SELECT coalesce(sum(calls), 0)
		FROM pg_wait_event_trace_by_statement($b_pn)
		WHERE wait_event_type = 'Lock' AND wait_event = 'relation'
		  AND bucket = '$bucket';
	));
}

# ---------------------------------------------------------------------
# (1) Bind of a named prepared statement blocks on the plan's lock.
# ---------------------------------------------------------------------
my $mark1 = ring_mark();
$psql_a->query_safe("BEGIN;\nLOCK TABLE pwet_pre_t IN ACCESS EXCLUSIVE MODE;");
$psql_b->query_until(qr/B_SENT_1/,
	"\\echo B_SENT_1\n\\bind_named pwet_pre_s \\g\n\\echo B_DONE_1\n");
wait_for_b_blocked();
select(undef, undef, undef, 0.2);
$psql_a->query_safe('COMMIT;');
$psql_b->query_until(qr/B_DONE_1/, '');

is(lock_rows($mark1), 'pre|ExecStart|true',
	'extended protocol: Bind-time lock wait is in the execution\'s '
	  . 'statement bucket, phase pre, query_id from its ExecStart');
is(lock_calls_in('<idle>'), '0', 'extended protocol: none in <idle>');

# ---------------------------------------------------------------------
# (2) Parse analysis of a simple-protocol query blocks on the table.
# ---------------------------------------------------------------------
my $mark2 = ring_mark();
$psql_a->query_safe("BEGIN;\nLOCK TABLE pwet_pre_t IN ACCESS EXCLUSIVE MODE;");
$psql_b->query_until(qr/B_SENT_2/,
	"\\echo B_SENT_2\nSELECT count(*) AS n FROM pwet_pre_t;\n\\echo B_DONE_2\n");
wait_for_b_blocked();
select(undef, undef, undef, 0.2);
$psql_a->query_safe('COMMIT;');
$psql_b->query_until(qr/B_DONE_2/, '');

is(lock_rows($mark2), 'pre|QueryStart|true',
	'simple protocol: parse-analysis lock wait is in the query\'s '
	  . 'statement bucket, phase pre, query_id from its QueryStart');
is(lock_calls_in('<idle>'), '0', 'simple protocol: none in <idle>');

# ---------------------------------------------------------------------
# (3) lock_timeout during Bind: the request fails before any statement.
# ---------------------------------------------------------------------
is(lock_calls_in('<no-statement>'), '0',
	'no <no-statement> wait before the failing request');
$psql_b->query_safe("SET lock_timeout = '100ms';");
my $mark3 = ring_mark();
$psql_a->query_safe("BEGIN;\nLOCK TABLE pwet_pre_t IN ACCESS EXCLUSIVE MODE;");
my ($out3, $err3) = $psql_b->query("\\bind_named pwet_pre_s \\g\n");
is($err3, 1, 'Bind with an expiring lock_timeout fails');
$psql_a->query_safe('COMMIT;');

cmp_ok(lock_calls_in('<no-statement>'), '>=', 1,
	'lock wait of the failed Bind is in <no-statement>');
is(lock_rows($mark3), '',
	'the failed Bind opened no statement bucket with a lock wait');
is( $node->safe_psql(
		'postgres', qq(
		SELECT count(*) FROM pg_wait_event_trace_by_statement($b_pn)
		WHERE bucket = '<no-statement>'
		  AND (phase IS NOT NULL OR statement_seq IS NOT NULL
		       OR query_id IS NOT NULL);
	)),
	'0',
	'<no-statement> rows carry no phase, statement_seq or query_id');

# ---------------------------------------------------------------------
# Invariant on the quiet session B: no wait lost or counted twice.
# ---------------------------------------------------------------------
$node->poll_query_until(
	'postgres', qq(
	SELECT wait_event = 'ClientRead' FROM pg_stat_activity WHERE pid = $b_pid;
)) or die "backend $b_pid did not return to ClientRead";
is( $node->safe_psql(
		'postgres', qq(
		SELECT (SELECT sum(calls) FROM pg_wait_event_trace_by_statement($b_pn))
		     = (SELECT count(*) FROM pg_get_wait_event_trace($b_pn)
		        WHERE wait_event_type <> 'Query');
	)),
	't',
	'sum(calls) equals the number of non-marker records of the ring');

$psql_a->quit;
$psql_b->quit;
$node->stop;
done_testing();
