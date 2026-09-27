
# Copyright (c) 2026, PostgreSQL Global Development Group
#
# An abort that cannot be recorded in a full ATM must not fail.
#
# ATM instant abort records the aborted transaction in the sLog radix tree,
# a DSA area capped by slog_dsa_max_size_mb.  With the logical revert worker
# held, every instant abort adds an entry until the cap is reached.  The
# registration runs inside AbortTransaction, so it must not raise an error
# there: the abort falls back (WARNING "ATM full"), the backend and server
# stay up, the aborted rows stay invisible, and they stay so after a crash
# restart.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# The cap is reached after about 4500 entries.
my $naborts = 8000;
my $nclients = 4;

my $node = PostgreSQL::Test::Cluster->new('atm_full_abort');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
logical_revert_naptime = 100
slog_dsa_max_size_mb = 2
undo_instant_abort_threshold = 0
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('amcheck'))
{
	plan skip_all => 'injection_points or amcheck not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION amcheck;
CREATE TABLE t (id int PRIMARY KEY, v int) USING heap
  WITH (index_undo = on, autovacuum_enabled = off);
INSERT INTO t VALUES (0, 0);
SELECT injection_points_attach('logical-revert-before-process', 'wait');
));

# The injection point holds the revert worker at its first entry, whenever
# it starts, so the ATM only grows.

my $log_offset = -s $node->logfile;

# Many aborted transactions, each an ATM instant abort, from several
# clients at once, until well past the cap.
my @clients;
for my $c (0 .. $nclients - 1)
{
	my $h = $node->background_psql('postgres', on_error_stop => 0);
	$h->query_safe('SET client_min_messages = error');
	$h->query_until(
		qr/start/, qq(\\echo start
DO \$\$
BEGIN
  FOR i IN 1..$naborts / $nclients LOOP
    INSERT INTO t VALUES (i * $nclients + $c, i);
    ROLLBACK;
  END LOOP;
END \$\$;
\\echo done
));
	push @clients, $h;
}
my $stderr = '';
for my $h (@clients)
{
	$h->query_until(qr/done/, '');
	$stderr .= $h->{stderr};
	$h->quit;
}

ok($node->log_contains(qr/WARNING: .*ATM full/, $log_offset),
	'full ATM reported as a WARNING');
unlike(
	$stderr,
	qr/ERROR|FATAL|PANIC|server closed/,
	'no error while aborting into a full ATM');
ok( !$node->log_contains(
		qr/ERROR|PANIC|server process .* was terminated/, $log_offset),
	'no ERROR or PANIC in the server log');

# The aborted rows stay invisible, through the index and the heap.
sub check_rows
{
	my ($what) = @_;
	is( $node->safe_psql(
			'postgres', qq(
SET enable_seqscan = off; SET enable_bitmapscan = off;
SELECT count(*) FROM t WHERE id BETWEEN -1 AND $naborts + $nclients;)),
		'1',
		"index scan sees only the committed row $what");
	is($node->safe_psql('postgres', 'SELECT count(*) FROM t'),
		'1', "seq scan sees only the committed row $what");
	$node->safe_psql('postgres', q(SELECT bt_index_check('t_pkey', true)));
	pass("t_pkey passes amcheck $what");
}
check_rows('after the aborts');

# Crash and restart: redo must not resurrect anything either.
$node->stop('immediate');
$node->start;
check_rows('after crash restart');

# The injection point did not survive the restart: the revert worker now
# drains what was recorded, and VACUUM removes the rest.
$node->safe_psql('postgres', 'VACUUM t');
check_rows('after VACUUM');

$node->stop;
done_testing();
