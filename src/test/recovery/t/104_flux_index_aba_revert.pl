# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX: an indexed key that goes A -> B -> A in a transaction that rolls
# back, when the rollback is not applied at once.
#
# A FLUX UPDATE of an indexed column keeps the row at its TID: it inserts or
# revives (k_new, TID) and delete-marks (k_old, TID).  The second UPDATE
# revives the (A, TID) the first one delete-marked.  Reverting the second
# UPDATE must put (A, TID) back the way the first one left it,
# delete-marked, for reverting the first one to make it live again; removing
# it instead loses the row from the index.
#
# undo_rollback_overflow_size = 0 sends the rollback to the undo apply
# workers, which are held at the injection point pbu-undo-worker-before-apply
# while the row is read, and while a writer runs into the aborted version and
# reverts it (helping).  Then the workers revert the rest.  Each FLUX table
# has a heap twin that gets the same statements; heap never reuses a TID, so
# it is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $node = PostgreSQL::Test::Cluster->new('flux_index_aba');
$node->init;
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
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
-- ids of the rows with k = key, by index scan and by seqscan
CREATE FUNCTION by_k(t regclass, key int, idx bool) RETURNS text
LANGUAGE plpgsql AS $$
DECLARE
  r text;
BEGIN
  PERFORM set_config('enable_seqscan', CASE WHEN idx THEN 'off' ELSE 'on' END, true);
  PERFORM set_config('enable_indexscan', CASE WHEN idx THEN 'on' ELSE 'off' END, true);
  PERFORM set_config('enable_bitmapscan', 'off', true);
  PERFORM set_config('enable_indexonlyscan', 'off', true);
  EXECUTE format('SELECT coalesce(string_agg(id::text, '','' ORDER BY id), '''') FROM %s WHERE k = $1', t)
    INTO r USING key;
  RETURN r;
END $$;
CREATE TABLE fx (id int PRIMARY KEY, j int, k int) USING flux;
CREATE TABLE hp (id int PRIMARY KEY, j int, k int) USING heap;
INSERT INTO fx SELECT g, 0, g FROM generate_series(1, 20) g;
INSERT INTO hp SELECT g, 0, g FROM generate_series(1, 20) g;
CREATE INDEX fx_k ON fx (k);
CREATE INDEX hp_k ON hp (k);
CREATE UNIQUE INDEX fx_uk ON fx (k);
CREATE UNIQUE INDEX hp_uk ON hp (k);
));

# Every key a row has held, for both tables and both scans.
my $agree = sub {
	my ($what, @keys) = @_;
	foreach my $key (@keys)
	{
		my $r = $node->safe_psql('postgres',
				"SELECT by_k('fx', $key, true), by_k('fx', $key, false), "
			  . "by_k('hp', $key, true), by_k('hp', $key, false)");
		my ($fi, $fs, $hi, $hs) = split /\|/, $r, -1;
		is("$fi|$fs|$hi", "$hs|$hs|$hs",
			"$what: k = $key: FLUX index scan, FLUX seqscan, heap index scan match heap seqscan"
		);
	}
};
my $amcheck = sub {
	my $what = shift;
	foreach my $i ('fx_k', 'fx_uk', 'fx_pkey')
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "SELECT bt_index_check('$i', true)");
		is($ret . $err, '0', "$what: bt_index_check($i, heapallindexed)");
	}
};
my $release = sub {
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('pbu-undo-worker-before-apply')");
	$node->poll_query_until('postgres', 'SELECT release_one()')
	  or die 'held undo apply workers were not released';
};

# T1: rows 12 and 13 go A -> B -> A; the last UPDATE also sets j to
# 16908801 = 0x01020201, which reads the same in either byte order, so the
# page can be searched for the aborted version.
my $pending =
  "SELECT position('\\x01020201'::bytea IN get_raw_page('fx', 0)) > 0";
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
foreach my $t ('fx', 'hp')
{
	$node->safe_psql(
		'postgres', qq(
BEGIN;
UPDATE $t SET k = k + 3000000 WHERE id IN (12, 13);
UPDATE $t SET k = k - 3000000, j = 16908801 WHERE id IN (12, 13);
ROLLBACK;
));
}
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
is($node->safe_psql('postgres', $pending), 't', "T1's rollback is pending");
$agree->('revert pending', 12, 13, 3000012, 3000013);

# A writer runs into row 13's aborted version and reverts it first.
$node->safe_psql('postgres', 'UPDATE fx SET j = 1 WHERE id = 13');
$node->safe_psql('postgres', 'UPDATE hp SET j = 1 WHERE id = 13');
$agree->('row 13 reverted by a writer', 12, 13, 3000012, 3000013);

# The worker reverts row 12.
$release->();
ok($node->poll_query_until('postgres', "SELECT NOT ($pending)"),
	"worker applied T1's rollback");
$agree->('worker applied the rollback', 12, 13, 3000012, 3000013);
$amcheck->('worker applied the rollback');

# Row 12's key is taken: no other row may take it, nor row 13's.
foreach my $t ('fx', 'hp')
{
	foreach my $key (12, 13)
	{
		my ($ret, $out, $err) =
		  $node->psql('postgres', "UPDATE $t SET k = $key WHERE id = 1");
		like(
			$err,
			qr/duplicate key value violates unique constraint/,
			"$t: k = $key is still taken");
	}
}

# And the same for a key moved away and back by a committed transaction,
# then by an aborted one whose rollback a writer reverts: the committed
# move leaves k = 14 free.
$node->safe_psql('postgres',
	"SELECT injection_points_attach('pbu-undo-worker-before-apply', 'wait')");
foreach my $t ('fx', 'hp')
{
	$node->safe_psql(
		'postgres', qq(
UPDATE $t SET k = 4000014 WHERE id = 14;
BEGIN;
UPDATE $t SET k = 14 WHERE id = 14;
UPDATE $t SET k = 4000014, j = 16908801 WHERE id = 14;
UPDATE $t SET k = 14 WHERE id = 14;
ROLLBACK;
));
}
$node->wait_for_event('undo apply worker', 'pbu-undo-worker-before-apply');
$agree->('committed move, aborted return pending', 14, 4000014);
$release->();
ok($node->poll_query_until('postgres', "SELECT NOT ($pending)"),
	"worker applied the aborted return");
$agree->('committed move, aborted return applied', 14, 4000014);
$amcheck->('committed move, aborted return applied');
foreach my $t ('fx', 'hp')
{
	is( $node->safe_psql(
			'postgres', "UPDATE $t SET k = 14 WHERE id = 2 RETURNING id"),
		'2',
		"$t: k = 14 is free once the row moved off it");
}
$agree->('another row took the freed key', 2, 14, 4000014);

$node->safe_psql('postgres', 'VACUUM fx, hp');
$agree->('after VACUUM', 2, 12, 13, 14, 3000012, 3000013, 4000014);
$amcheck->('after VACUUM');

$node->stop;
done_testing();
