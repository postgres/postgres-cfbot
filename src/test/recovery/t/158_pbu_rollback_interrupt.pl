# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A long per-backend rollback must answer a shutdown, terminate or cancel
# request, and whatever it did not finish must be finished later.
#
# execute_undo_actions() applies a rollback in batches of
# maintenance_work_mem.  An undo apply worker that did not check for
# interrupts between them held up a fast shutdown, and ignored
# pg_terminate_backend(), until the whole rollback was applied.  A backend
# finishing a rollback after its transaction (PbuApplyPendingUndo) ignored a
# cancel in the same way.  The injection point pbu-undo-apply-batch is hit
# before each batch; test_idxundo's idxundo_sleep makes each batch take
# seconds, so the rollback left when the request arrives takes several times
# longer than the bound the request must be answered in.  shared_buffers
# holds every page, so that no buffer read checks for interrupts on the
# rollback's behalf.  Rows changed by
# the aborted transactions carry 16908801 = 0x01020201, which reads the same
# in either byte order, so the raw tuples show whether a rollback is still
# outstanding.  A heap twin runs the same statements.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $X = 16908801;
my $rows = 10000;
my $sleep_ms = 3000;    # per batch: the rollback takes minutes
my $bound = 30;         # seconds a request may take to be answered

my $node = PostgreSQL::Test::Cluster->new('pbu_interrupt');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
max_wal_size = 4GB
# no buffer reads during the rollback
shared_buffers = 128MB
# small batches: a rollback of $rows rows takes many of them
maintenance_work_mem = 64kB
# every rollback goes to an undo apply worker
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('pageinspect')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'injection_points, pageinspect or amcheck not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
CREATE EXTENSION amcheck;
));

# Does a live tuple of the table still hold an aborted value?  (The bytes
# of a version the rollback removed may stay behind in the page's free
# space.)
sub pending
{
	my ($t) = @_;
	return 't' eq $node->safe_psql(
		'postgres', qq(
SELECT count(*) > 0
  FROM generate_series(0,
         pg_relation_size('$t') / current_setting('block_size')::int - 1) b,
       LATERAL (SELECT get_raw_page('$t', b::int) AS p) r,
       LATERAL heap_page_items(r.p) i
 WHERE i.lp_flags = 1
   AND position('\\x01020201'::bytea IN substr(r.p, i.lp_off + 1, i.lp_len)) > 0)
	);
}

# Is every rollback of permanent undo applied?  The discard worker cannot
# discard past one that is not.
my $drained = q(
SELECT (SELECT count(*) FROM pg_stat_get_undo_retention()) = 0
   AND (SELECT coalesce(bool_and(discard = insert), true)
        FROM pg_stat_get_undo_logs() WHERE persistence = 'permanent'));

my $slow = qq(
SELECT injection_points_attach('pbu-undo-apply-batch', 'test_idxundo',
  'idxundo_sleep', '$sleep_ms'::bytea));
my $fast = q(SELECT injection_points_detach('pbu-undo-apply-batch'));

# Seconds until $cond returns true, or undef if it does not within $limit
# seconds.
sub answered_within
{
	my ($cond, $limit) = @_;
	my $start = time;
	while (time - $start < $limit)
	{
		return time - $start if $cond->();
		usleep(100_000);
	}
	return undef;
}

sub log_has
{
	my ($re, $offset) = @_;
	return substr(slurp_file($node->logfile), $offset) =~ $re;
}

# Check that a request was answered in time.  Then let the rollback go on at
# full speed, unless it was answered and $keep_slow is set.
sub check_answered
{
	my ($took, $what, $keep_slow) = @_;
	ok( defined $took && $took < $bound,
		defined $took
		? sprintf("$what answered in %.1f s", $took)
		: "$what not answered in " . 2 * $bound . " s");
	$node->safe_psql('postgres', $fast)
	  unless defined $took && $keep_slow;
	return defined $took;
}

# The FLUX table holds what its heap twin holds, and once its rollback is
# complete ($check), its primary key is intact.
sub same_as_heap
{
	my ($p, $what, $check) = @_;
	my $q =
	  "SELECT count(*) || ':' || sum(k::bigint) || ':' || sum(v::bigint) FROM T";
	my @res;
	foreach my $am ('flux', 'heap')
	{
		(my $sq = $q) =~ s/\bT\b/${p}_$am/;
		push @res,
		  $node->safe_psql('postgres',
			"SET enable_indexscan = off; SET enable_bitmapscan = off; $sq;\n"
			  . "SET enable_seqscan = off; SET enable_indexscan = on; $sq WHERE k > 0;"
		  );
	}
	is($res[0], $res[1], "${p}_flux: rows as heap, $what");
	return unless $check;
	my ($ret, $out, $err) = $node->psql('postgres',
		"SELECT bt_index_check('${p}_flux_pkey', heapallindexed => true)");
	is("$ret:$err", '0:', "${p}_flux_pkey: bt_index_check, $what");
}

# Abort a key UPDATE of every row of ${p}_flux and its heap twin.
sub make_tables
{
	my ($p) = @_;
	foreach my $am ('flux', 'heap')
	{
		$node->safe_psql(
			'postgres', qq(
CREATE TABLE ${p}_$am (k int PRIMARY KEY, v int) USING $am;
INSERT INTO ${p}_$am SELECT g, g FROM generate_series(1, $rows) g;
));
	}
}

sub abort_update
{
	my ($p) = @_;
	foreach my $am ('flux', 'heap')
	{
		$node->safe_psql('postgres',
			"BEGIN; UPDATE ${p}_$am SET k = k + 1000000, v = $X; ROLLBACK;");
	}
}

# Hold the undo apply worker that takes the next rollback at its first batch,
# then let it go on slowly.  Returns once it has applied a batch and sleeps
# before the next one; returns its pid.
sub worker_mid_rollback
{
	my ($p) = @_;
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('pbu-undo-apply-batch', 'wait')");
	abort_update($p);
	$node->wait_for_event('undo apply worker', 'pbu-undo-apply-batch');
	$node->safe_psql('postgres', "$fast; $slow");
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('pbu-undo-apply-batch')");
	$node->wait_for_event('undo apply worker', 'PgSleep');
	ok(pending("${p}_flux"), "$p: the rollback is under way");
	return $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE backend_type = 'undo apply worker' AND wait_event = 'PgSleep'"
	);
}

###########################################################################
# A fast shutdown while an undo apply worker is in the middle of a rollback.
###########################################################################
make_tables('s');
worker_mid_rollback('s');
my $start = time;
my $stopped = $node->stop('fast', fail_ok => 1, timeout => $bound);
my $took = time - $start;
note sprintf("fast shutdown: %.1f s", $took);
ok($stopped, "fast shutdown completed during a rollback");
cmp_ok($took, '<', $bound,
	sprintf("fast shutdown took %.1f s, under $bound s", $took));
$node->stop('immediate') unless $stopped;

# Restart without room for an undo apply worker (as in 076): max_worker_processes
# is just the static background workers.  The rollback is still outstanding.
$node->append_conf(
	'postgresql.conf', q(
max_logical_replication_workers = 0
max_logical_revert_workers = 0
max_parallel_workers = 0
max_parallel_workers_per_gather = 0
max_worker_processes = 3
));
my $log_offset = -s $node->logfile;
$node->start;
unlike(
	substr(slurp_file($node->logfile), $log_offset),
	qr/too many background workers/,
	'every static background worker registered');
is( $node->safe_psql(
		'postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'undo apply worker'"
	),
	'0',
	'no undo apply worker');
ok(pending('s_flux'), 's: the rollback is outstanding after the restart');
same_as_heap('s', 'rollback outstanding', 0);

# Now let the undo apply workers run: the rollback is finished.
$node->append_conf(
	'postgresql.conf', q(
max_logical_replication_workers = 4
max_logical_revert_workers = 2
max_parallel_workers = 8
max_parallel_workers_per_gather = 2
max_worker_processes = 8
));
$node->restart;
ok($node->poll_query_until('postgres', $drained),
	's: every rollback applied');
ok(!pending('s_flux'), 's: the rollback is complete');
same_as_heap('s', 'after the restart', 1);

###########################################################################
# pg_terminate_backend() and pg_cancel_backend() of an undo apply worker in
# the middle of a rollback: it stops, and another worker takes the rollback
# up.  A cancel is not a failed attempt.
###########################################################################
make_tables('t');
foreach my $how ('terminate', 'cancel')
{
	$log_offset = -s $node->logfile;
	my $pid = worker_mid_rollback('t');
	$node->safe_psql('postgres', "SELECT pg_${how}_backend($pid)");
	check_answered(
		answered_within(
			$how eq 'terminate'
			? sub {
				$node->safe_psql('postgres',
					"SELECT count(*) FROM pg_stat_activity WHERE pid = $pid")
				  eq '0';
			}
			: sub {
				log_has(
					qr/\[$pid\] ERROR:  canceling statement due to user request/,
					$log_offset);
			},
			2 * $bound),
		"t: pg_${how}_backend of the worker");
	ok( $node->poll_query_until('postgres', $drained),
		"t: every rollback applied after pg_${how}_backend");
	ok(!pending('t_flux'),
		"t: the rollback is complete after pg_${how}_backend");
	same_as_heap('t', "after pg_${how}_backend", 1);
	unlike(
		substr(slurp_file($node->logfile), $log_offset),
		qr/giving up on rolling back/,
		"t: pg_${how}_backend did not make the rollback be given up");
}

###########################################################################
# A backend finishing a rollback after its transaction (PbuApplyPendingUndo):
# the transaction's own rewrite kept its abort from applying the undo.  A
# cancel stops it and leaves the rollback pending, not given up; a terminate
# hands it to the undo apply workers.
###########################################################################
$node->safe_psql('postgres',
	"ALTER SYSTEM SET undo_rollback_overflow_size = 16; SELECT pg_reload_conf()"
);
make_tables('b');
my $bg = $node->background_psql('postgres', on_error_stop => 0);
my $bpid = $bg->query_safe('SELECT pg_backend_pid()');
$bg->query_safe(
	"BEGIN; UPDATE b_flux SET v = $X; ALTER TABLE b_flux ALTER COLUMN v TYPE bigint;"
);
$node->safe_psql('postgres',
	"BEGIN; UPDATE b_heap SET v = $X; ALTER TABLE b_heap ALTER COLUMN v TYPE bigint; ROLLBACK;"
);

# The abort leaves the undo unapplied: the rewrite gave the table another
# file.  Hold the backend once its transaction is over, then let it finish
# the rollback slowly.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-apply-pending-undo', 'wait')");
$bg->query_until(qr/go1/, "\\echo go1\nROLLBACK;\n");
$node->wait_for_event('client backend', 'pbu-apply-pending-undo');
$node->safe_psql(
	'postgres', qq($slow;
SELECT injection_points_detach('pbu-apply-pending-undo');
SELECT injection_points_wakeup('pbu-apply-pending-undo');));

# The backend tries the rollback again when its next command starts and
# before it reports ready for the next one.  Make it try again unless it is
# already at it; return once it is in the middle of it.
my $tries = 1;

sub backend_mid_rollback
{
	$node->poll_query_until('postgres',
		"SELECT state = 'idle' OR wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid = $bpid"
	) or die "backend did not settle";
	if ($node->safe_psql('postgres',
			"SELECT state FROM pg_stat_activity WHERE pid = $bpid") eq 'idle')
	{
		$tries++;
		$bg->query_until(qr/go$tries/, "\\echo go$tries\nSELECT 1;\n");
	}
	$node->poll_query_until('postgres',
		"SELECT wait_event = 'PgSleep' FROM pg_stat_activity WHERE pid = $bpid"
	) or die "backend did not start the rollback";
}

# Three cancels: as many as the attempts the backend makes before it gives
# up on the rollback, if a cancel counted as one.  A rollback that does not
# answer a cancel runs to completion, so stop there.
my $answered = 1;
foreach my $i (1 .. 3)
{
	backend_mid_rollback();
	$log_offset = -s $node->logfile;
	$node->safe_psql('postgres', "SELECT pg_cancel_backend($bpid)");
	$answered = check_answered(
		answered_within(
			sub {
				log_has(
					qr/WARNING:  could not finish rolling back transaction \d+: canceling statement due to user request/,
					$log_offset);
			},
			2 * $bound),
		"b: cancel $i of the backend's rollback",
		1);
	last unless $answered;
	ok(pending('b_flux'), "b: the rollback is pending after cancel $i");
}
same_as_heap('b', 'rollback pending in the backend', 0) if $answered;

# A terminate: the undo apply workers finish the rollback.
if ($answered)
{
	backend_mid_rollback();
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($bpid)");
	check_answered(
		answered_within(
			sub {
				$node->safe_psql('postgres',
					"SELECT count(*) FROM pg_stat_activity WHERE pid = $bpid")
				  eq '0';
			},
			2 * $bound),
		"b: pg_terminate_backend of the backend");
}
$bg->{run}->kill_kill;
ok($node->poll_query_until('postgres', $drained),
	'b: every rollback applied after the backend was terminated');
ok(!pending('b_flux'), 'b: the rollback is complete');
same_as_heap('b', 'after the undo apply worker', 1);

unlike(
	slurp_file($node->logfile),
	qr/TRAP|PANIC|does not match aborted version|giving up on rolling back/,
	'no failed assertion, mismatched or abandoned undo');
$node->stop;
done_testing();
