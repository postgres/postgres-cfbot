# Copyright (c) 2026, PostgreSQL Global Development Group
#
# Per-backend UNDO written inside subtransactions must be rolled back by the
# enclosing (sub)transaction: undo of a RELEASEd savepoint belongs to its
# parent, both for a top-level ROLLBACK and for ROLLBACK TO an outer savepoint.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_subxact');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

foreach my $am ('flux')
{
	my $t = "sx_$am";

	$node->safe_psql(
		'postgres', qq(
CREATE TABLE $t (id int, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 20) g;
));

	my %cases = (
		'only a released savepoint writes' => qq(
BEGIN;
SAVEPOINT s;
UPDATE $t SET v = 'x' WHERE id = 1;
RELEASE s;
ROLLBACK;),
		'top, released savepoint, top' => qq(
BEGIN;
UPDATE $t SET v = 'x' WHERE id = 2;
SAVEPOINT s;
UPDATE $t SET v = 'x' WHERE id = 3;
RELEASE s;
UPDATE $t SET v = 'x' WHERE id = 4;
ROLLBACK;),
		'released savepoint writes first, then top' => qq(
BEGIN;
SAVEPOINT s;
UPDATE $t SET v = 'x' WHERE id = 5;
RELEASE s;
UPDATE $t SET v = 'x' WHERE id = 6;
ROLLBACK;),
		'nested released savepoints' => qq(
BEGIN;
SAVEPOINT a;
SAVEPOINT b;
UPDATE $t SET v = 'x' WHERE id = 7;
RELEASE b;
UPDATE $t SET v = 'x' WHERE id = 8;
RELEASE a;
ROLLBACK;),
		'ROLLBACK TO outer after RELEASE inner' => qq(
BEGIN;
SAVEPOINT p;
SAVEPOINT c;
UPDATE $t SET v = 'x' WHERE id = 9;
RELEASE c;
ROLLBACK TO p;
COMMIT;),
		'PL/pgSQL exception block that completes' => qq(
BEGIN;
DO \$\$ BEGIN
  BEGIN
    UPDATE $t SET v = 'x' WHERE id = 10;
  EXCEPTION WHEN others THEN NULL;
  END;
END \$\$;
ROLLBACK;),);

	foreach my $name (sort keys %cases)
	{
		my ($ret, $out, $err) = $node->psql('postgres', $cases{$name});
		is($ret, 0, "$am: $name: no error") or diag($err);
		is( $node->safe_psql(
				'postgres', "SELECT count(*) FROM $t WHERE v = 'orig' || id"),
			'20',
			"$am: $name: every row restored");
	}
}

# Undo that ROLLBACK TO SAVEPOINT already applied must never be applied
# again.  Once the subtransaction has rolled back and released its row locks,
# another transaction may update and commit the same row; re-applying the
# subtransaction's before-image would silently overwrite that committed value.
# Three variants cover the three ways the rest of the chain gets applied: the
# aborting backend itself, an undo apply worker, and crash recovery.
sub n1_setup
{
	my ($t, $am) = @_;
	$node->safe_psql(
		'postgres', qq(
DROP TABLE IF EXISTS $t;
CREATE TABLE $t (id int, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 3) g;
));
}

# T1 writes, rolls back a savepoint that wrote row 2, and keeps running while
# T2 updates row 2 and commits.  Returns T1's session.
sub n1_interleave
{
	my ($t) = @_;
	my $t1 = $node->background_psql('postgres');
	$t1->query_safe(
		qq(BEGIN;
UPDATE $t SET v = 't1' WHERE id = 1;
SAVEPOINT s;
UPDATE $t SET v = 't1-sub' WHERE id = 2;
ROLLBACK TO s;
RELEASE s;));
	$node->safe_psql('postgres', "UPDATE $t SET v = 't2' WHERE id = 2");
	return $t1;
}

sub n1_check
{
	my ($t, $what) = @_;
	is( $node->safe_psql(
			'postgres', "SELECT string_agg(v, ',' ORDER BY id) FROM $t"),
		'orig1,t2,orig3',
		$what);
}

foreach my $am ('flux')
{
	my $t = "n1_$am";

	# Inline: T1 writes again after ROLLBACK TO, then rolls back.
	n1_setup($t, $am);
	my $t1 = n1_interleave($t);
	$t1->query_safe("UPDATE $t SET v = 't1' WHERE id = 3; ROLLBACK;");
	$t1->quit;
	n1_check($t,
		"$am: ROLLBACK keeps a committed update made after ROLLBACK TO");

	# Worker: every rollback request goes to an undo apply worker, which
	# derives the end of the chain from the undo log's insert pointer.
	$node->safe_psql('postgres',
		'ALTER SYSTEM SET undo_rollback_overflow_size = 0');
	$node->reload;
	n1_setup($t, $am);
	$t1 = n1_interleave($t);
	$t1->query_safe('ROLLBACK');
	$t1->quit;
	$node->poll_query_until('postgres',
		"SELECT v = 'orig1' FROM $t WHERE id = 1")
	  or diag("$am: worker never rolled back T1");
	n1_check($t,
		"$am: worker rollback keeps a committed update made after ROLLBACK TO"
	);
	$node->safe_psql('postgres',
		'ALTER SYSTEM RESET undo_rollback_overflow_size');
	$node->reload;

	# Crash recovery: T1 is still running when the server crashes.
	n1_setup($t, $am);
	$t1 = n1_interleave($t);
	$node->stop('immediate');
	$t1->{run}->kill_kill;
	$node->start;
	$node->poll_query_until('postgres',
		"SELECT v = 'orig1' FROM $t WHERE id = 1")
	  or diag("$am: crashed T1 was never rolled back");
	n1_check($t,
		"$am: crash recovery keeps a committed update made after ROLLBACK TO"
	);
}

# A subtransaction that writes tables of several persistence levels has an
# undo chain in each level's undo log.  ROLLBACK TO SAVEPOINT must apply all
# of them, whichever level it wrote first.  Nothing of them may be left for
# later: a top-level ROLLBACK walks only the parent's own records, and after
# a COMMIT the undo is discarded, so a change left in place survives or the
# row is lost.
my @mixed;
foreach my $am ('flux')
{
	foreach my $order ('p u t', 'u t p', 't p u')
	{
		foreach my $end ('COMMIT', 'ROLLBACK')
		{
			my $sfx = "${am}_" . join('', split(/ /, $order)) . "_$end";
			my $upd = join("\n",
				map { "UPDATE m${_}_$sfx SET v = 'sub';" }
				  split(/ /, $order));
			my $cnt = join(
				" || ',' || ",
				map {
					"(SELECT count(*) FROM m${_}_$sfx WHERE v = 'orig' || id)"
				} ('p', 'u', 't'));
			my $out = $node->safe_psql(
				'postgres', qq(
CREATE TABLE mp_$sfx (id int, v text) USING $am;
CREATE UNLOGGED TABLE mu_$sfx (id int, v text) USING $am;
CREATE TEMP TABLE mt_$sfx (id int, v text) USING $am;
INSERT INTO mp_$sfx SELECT g, 'orig' || g FROM generate_series(1, 10) g;
INSERT INTO mu_$sfx SELECT g, 'orig' || g FROM generate_series(1, 10) g;
INSERT INTO mt_$sfx SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
SAVEPOINT s;
$upd
ROLLBACK TO s;
SELECT $cnt;
$end;
SELECT $cnt;
));
			is($out, "10,10,10\n10,10,10",
				"$am: ROLLBACK TO restores every level (order $order), then $end"
			);
			push @mixed, $sfx;
		}
	}
}

# A savepoint that updates rows and then truncates or drops the table.  When
# the subtransaction's undo is first applied, the relation still looks
# dropped or empty, so the records cannot be applied then.  They must be
# applied once the subtransaction's catalog changes are rolled back, before
# the transaction goes on: it commits with every committed row intact.  The
# variants reach that through ROLLBACK TO, ROLLBACK TO after an error,
# ROLLBACK TO an outer savepoint, and a PL/pgSQL exception block.
my %sd = (
	rt => sub {
		my ($t, $op) = @_;
		"SAVEPOINT s;\nUPDATE $t SET v = 'sub';\n$op $t;\nROLLBACK TO s;";
	},
	rterr => sub {
		my ($t, $op) = @_;
		"SAVEPOINT s;\nUPDATE $t SET v = 'sub';\n$op $t;\nSELECT 1/0;\nROLLBACK TO s;";
	},
	outer => sub {
		my ($t, $op) = @_;
		"SAVEPOINT a;\nSAVEPOINT b;\nUPDATE $t SET v = 'sub';\n$op $t;\nROLLBACK TO a;";
	},
	plpgsql => sub {
		my ($t, $op) = @_;
		"DO \$\$ BEGIN\n  BEGIN\n    UPDATE $t SET v = 'sub';\n    $op $t;\n"
		  . "    RAISE EXCEPTION 'x';\n  EXCEPTION WHEN raise_exception THEN NULL;\n"
		  . "  END;\nEND \$\$;";
	},);
foreach my $am ('flux')
{
	foreach my $op ('TRUNCATE', 'DROP TABLE')
	{
		foreach my $how (sort keys %sd)
		{
			my $t = "sd_${am}_" . lc((split(/ /, $op))[0]) . "_$how";
			$node->safe_psql(
				'postgres', qq(
CREATE TABLE $t (id int, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 10) g;
));
			my ($ret, $out, $err) = $node->psql(
				'postgres', qq(
BEGIN;
@{[ $sd{$how}->($t, $op) ]}
SELECT count(*) FROM $t WHERE v = 'orig' || id;
COMMIT;
SELECT count(*) FROM $t WHERE v = 'orig' || id;
),
				on_error_stop => 0);
			is($out, "10\n10",
				"$am: $op in a savepoint ($how): rows restored")
			  or diag($err);
			unlike($err, qr/incomplete/,
				"$am: $op in a savepoint ($how): commits");
			push @mixed, $t;
		}
	}
}

# Committed or rolled back, the undo is gone after a restart; the rows must
# not depend on it.
$node->restart;
foreach my $sfx (@mixed)
{
	my $q =
	  $sfx =~ /^sd_/
	  ? "SELECT count(*) FROM $sfx WHERE v = 'orig' || id"
	  : qq(SELECT (SELECT count(*) FROM mp_$sfx WHERE v = 'orig' || id) || ',' ||
       (SELECT count(*) FROM mu_$sfx WHERE v = 'orig' || id));
	is( $node->safe_psql('postgres', $q),
		$sfx =~ /^sd_/ ? '10' : '10,10',
		"$sfx: rows intact after restart");
}

$node->stop;
done_testing();
