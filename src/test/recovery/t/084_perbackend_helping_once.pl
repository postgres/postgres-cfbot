# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Every per-backend undo record is applied exactly once, whoever applies it:
# ROLLBACK TO SAVEPOINT (which marks what it applied), a writer that runs
# into an aborted version (helping), the undo apply worker, and crash
# recovery.  Here all of them work on one aborted transaction, T1, whose
# rows other transactions update and commit before the worker gets to it.
# Applying any record twice would put an old image back over a committed
# value.
#
# undo_rollback_overflow_size = 0 sends T1's rollback to the undo apply
# workers, which are held at the injection point pbu-undo-worker-before-apply
# until the end.  T1 has an undo chain in the permanent and in the unlogged
# undo log, each a rollback request of its own, so there can be more than one
# worker to release.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('pbu_helping');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
undo_rollback_overflow_size = 0
shared_preload_libraries = 'injection_points'
));
$node->start;
if (   !$node->check_extension('injection_points')
	|| !$node->check_extension('pageinspect'))
{
	plan skip_all => 'injection_points or pageinspect not installed';
}
$node->safe_psql(
	'postgres', q(
CREATE EXTENSION injection_points;
CREATE EXTENSION pageinspect;
SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait');
-- Wake one held undo apply worker; true once none is held any more.
CREATE FUNCTION release_one() RETURNS bool LANGUAGE plpgsql AS $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_stat_activity
                 WHERE wait_event = 'pbu-undo-worker-before-apply') THEN
    RETURN true;
  END IF;
  BEGIN
    PERFORM injection_points_wakeup('pbu-undo-worker-before-apply');
  EXCEPTION WHEN OTHERS THEN
    NULL;  -- it left the injection point meanwhile
  END;
  RETURN false;
END $$;
));

my @s =
  map { $node->background_psql('postgres', on_error_stop => 0) } (1 .. 3);

my @tabs;
foreach my $am ('flux')
{
	foreach my $pers ('', 'UNLOGGED')
	{
		my $t = "h_${am}_" . ($pers ? 'u' : 'p');
		push @tabs, $t;
		$s[0]->query_safe(
			qq(
CREATE $pers TABLE $t (id int PRIMARY KEY, v int) USING $am;
INSERT INTO $t SELECT g, g FROM generate_series(1, 6) g;
));
	}
}

# T1: rows 1-3 at the top level; rows 2 and 4 in a savepoint that is rolled
# back at once (and released, so that what follows is top-level again); row
# 5 after it; then the whole transaction rolls back.  Row
# 5's aborted value, 16908801 = 0x01020201, reads the same in either byte
# order, so the page can be searched for it.
my $each = sub {
	my $sql = shift;
	return join("\n", map { (my $x = $sql) =~ s/\bT\b/$_/g; $x } @tabs);
};
$s[0]->query_safe("BEGIN;\n"
	  . $each->('UPDATE T SET v = v + 100 WHERE id IN (1, 2, 3);')
	  . "\nSAVEPOINT s;\n"
	  . $each->('UPDATE T SET v = v + 1000 WHERE id IN (2, 4);')
	  . "\nROLLBACK TO s;\nRELEASE s;\n"
	  . $each->('SELECT string_agg(v::text, \',\' ORDER BY id) FROM T;')
	  . "\n"
	  . $each->('UPDATE T SET v = 16908801 WHERE id = 5;')
	  . "\nROLLBACK;");

my $pending = sub {
	my $t = shift;
	return
	  "SELECT position('\\x01020201'::bytea IN get_raw_page('$t', 0)) > 0";
};
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
foreach my $t (@tabs)
{
	is($s[1]->query_safe($pending->($t)),
		't', "$t: T1's rollback is still pending");
}

# T2 updates rows 1, 2 and 4 and commits.  Rows 1 and 2 carry T1's aborted
# top-level images, which T2 reverts first (helping); row 4 was already
# restored, and marked applied, by ROLLBACK TO.  Row 2 has both: a record
# T1's ROLLBACK TO applied and marked, under one helping reverts.  T3 helps row 3 inside a savepoint that it rolls
# back, then commits an update of row 6.
$s[1]->query_safe($each->('UPDATE T SET v = v + 20 WHERE id IN (1, 2, 4);'));
$s[2]->query_safe("BEGIN;\nSAVEPOINT a;\n"
	  . $each->('UPDATE T SET v = v + 7000 WHERE id = 3;')
	  . "\nROLLBACK TO a;\n"
	  . $each->('UPDATE T SET v = v + 30 WHERE id = 6;')
	  . "\nCOMMIT;");

my $want = '21,22,3,24,5,36';
my $rows = sub {
	my $t = shift;
	return "SELECT string_agg(v::text, ',' ORDER BY id) FROM $t";
};
foreach my $t (@tabs)
{
	is($s[1]->query_safe($rows->($t)),
		$want, "$t: helpers' committed values over T1's pending rollback");
}

# Let the held undo apply workers go.  injection_points_wakeup() releases
# one waiter per call.
my $release = sub {
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
	$node->poll_query_until('postgres', 'SELECT release_one()')
	  or die 'held undo apply workers were not released';
};

# Now the workers apply the rest of T1: row 5, and nothing else.
$release->();
foreach my $t (@tabs)
{
	ok( $node->poll_query_until(
			'postgres', "SELECT NOT (" . $pending->($t) . ")"),
		"$t: worker applied T1's remaining record");
	is($node->safe_psql('postgres', $rows->($t)),
		$want, "$t: worker left the committed values alone");
}

# ROLLBACK TO a savepoint that wrote nothing reverts nothing: not the
# parent's records, whose rollback is left to the worker, nor, in a
# transaction that commits, the parent's changes.  ROLLBACK TO restarts the
# savepoint, so what T4 writes after it is the savepoint's, and the
# top-level ROLLBACK applies that itself when it aborts the savepoint.  Row
# 6's aborted value 16974593 = 0x01030301 is searchable like row 5's.
my $pending6 = sub {
	my $t = shift;
	return
	  "SELECT position('\\x01030301'::bytea IN get_raw_page('$t', 0)) > 0";
};
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
my $t4 = $node->safe_psql('postgres',
		"BEGIN;\n"
	  . $each->('UPDATE T SET v = 16908801 WHERE id = 5;')
	  . "\nSAVEPOINT s;\nROLLBACK TO s;\n"
	  . $each->("SELECT 'T', v FROM T WHERE id = 5;") . "\n"
	  . $each->('UPDATE T SET v = 16974593 WHERE id = 6;')
	  . "\nROLLBACK;");
is( $t4,
	join("\n", map { "$_|16908801" } @tabs),
	'ROLLBACK TO an empty savepoint keeps the parent\'s change');
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
foreach my $t (@tabs)
{
	is($node->safe_psql('postgres', $pending->($t)),
		't', "$t: the parent's record is left to the worker");
	is($node->safe_psql('postgres', $pending6->($t)),
		'f', "$t: the savepoint's record is applied by the ROLLBACK");
}
$release->();
foreach my $t (@tabs)
{
	ok( $node->poll_query_until(
			'postgres', "SELECT NOT (" . $pending->($t) . ")"),
		"$t: worker applied the parent's record");
	is($node->safe_psql('postgres', $rows->($t)),
		$want, "$t: T4 left no trace");
}
$node->safe_psql('postgres',
		"BEGIN;\n"
	  . $each->('UPDATE T SET v = v + 1 WHERE id = 1;')
	  . "\nSAVEPOINT s;\nROLLBACK TO s;\nCOMMIT;");
foreach my $t (@tabs)
{
	is( $node->safe_psql('postgres', $rows->($t)),
		'22,22,3,24,5,36',
		"$t: a commit after ROLLBACK TO an empty savepoint keeps the change");
}
$want = '22,22,3,24,5,36';

$_->quit for @s;

$node->restart;
foreach my $t (@tabs)
{
	is($node->safe_psql('postgres', $rows->($t)), $want, "$t: after restart");
}

# Crash recovery finds nothing left to apply either.
$node->stop('immediate');
$node->start;
foreach my $t (grep { /_p$/ } @tabs)
{
	is($node->safe_psql('postgres', $rows->($t)), $want, "$t: after crash");
}
unlike(
	slurp_file($node->logfile),
	qr/cannot restore|TRAP|PANIC/,
	'no failed restore');

$node->stop;
done_testing();
