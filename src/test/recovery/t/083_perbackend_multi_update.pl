# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A row updated several times in one transaction has one undo record per
# update, each restoring the image the one before it wrote.  A rollback must
# apply all of them, newest first, and end at the committed image: inline, by
# an undo apply worker, by crash recovery, and by ROLLBACK TO SAVEPOINT; for
# permanent, unlogged and temporary FLUX tables.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('pbu_multi_update');
$node->init;
$node->append_conf('postgresql.conf', 'autovacuum = off');
$node->start;

my %kw = (p => '', u => 'UNLOGGED', t => 'TEMP');

# Create the table, update every row $n times in one transaction (the
# updates grow the row, then shrink it), then run $end.  Returns the number
# of rows that hold the committed value.
sub run_case
{
	my ($am, $pers, $n, $body_end) = @_;
	my $t = "mu_${am}_$pers";
	my @upd =
	  map { "UPDATE $t SET v = repeat('u$_', " . ($_ == 2 ? 50 : $_) . ');' }
	  (1 .. $n);
	return $node->safe_psql(
		'postgres', qq(
CREATE $kw{$pers} TABLE $t (id int PRIMARY KEY, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
@{[ $body_end->($t, join("\n", @upd)) ]}
SELECT count(*) FROM $t WHERE v = 'orig' || id;
DROP TABLE $t;
));
}

my %ends = (
	'ROLLBACK' => sub { my ($t, $u) = @_; "$u\nROLLBACK;" },
	'ROLLBACK TO + COMMIT' =>
	  sub { my ($t, $u) = @_; "SAVEPOINT s;\n$u\nROLLBACK TO s;\nCOMMIT;" },
	'ROLLBACK TO + ROLLBACK' =>
	  sub { my ($t, $u) = @_; "SAVEPOINT s;\n$u\nROLLBACK TO s;\nROLLBACK;" },
	'one update before the savepoint, the rest after; ROLLBACK TO + COMMIT'
	  => sub {
		my ($t, $u) = @_;
		my ($first, @rest) = split(/\n/, $u);
		"$first\nSAVEPOINT s;\n@{[ join(qq(\n), @rest) ]}\nROLLBACK TO s;\n"
		  . "UPDATE $t SET v = 'orig' || id;\nCOMMIT;";
	  },);

foreach my $overflow (16, 0)
{
	$node->safe_psql('postgres',
		"ALTER SYSTEM SET undo_rollback_overflow_size = $overflow");
	$node->reload;
	foreach my $am ('flux')
	{
		foreach my $pers ('p', 'u', 't')
		{
			foreach my $n (2, 3)
			{
				foreach my $end (sort keys %ends)
				{
					my $name =
					  "$am $pers, $n updates, $end, overflow $overflow";

					# A worker applies the rollback asynchronously: wait for
					# it rather than reading the table at once.
					if ($overflow == 0 && $pers ne 't' && $end eq 'ROLLBACK')
					{
						my $t = "mw_${am}_$pers$n";
						my @upd = map {
							"UPDATE $t SET v = repeat('u$_', "
							  . ($_ == 2 ? 50 : $_) . ');'
						} (1 .. $n);
						$node->safe_psql(
							'postgres', qq(
CREATE $kw{$pers} TABLE $t (id int PRIMARY KEY, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 10) g;
BEGIN;
@upd
ROLLBACK;
));
						ok( $node->poll_query_until(
								'postgres',
								"SELECT count(*) = 10 FROM $t WHERE v = 'orig' || id"
							),
							$name);
						next;
					}
					is(run_case($am, $pers, $n, $ends{$end}), '10', $name);
				}
			}
		}
	}
}
$node->safe_psql('postgres',
	'ALTER SYSTEM RESET undo_rollback_overflow_size');
$node->reload;

# Crash recovery: the transaction is still running when the server crashes.
# Permanent tables only; unlogged tables are reset by the crash.
foreach my $am ('flux')
{
	foreach my $n (2, 3)
	{
		my $t = "mc_${am}_$n";
		$node->safe_psql(
			'postgres', qq(
CREATE TABLE $t (id int PRIMARY KEY, v text) USING $am;
INSERT INTO $t SELECT g, 'orig' || g FROM generate_series(1, 10) g;
));
		my $s = $node->background_psql('postgres');
		$s->query_safe('BEGIN;');
		$s->query_safe(
			"UPDATE $t SET v = repeat('u$_', " . ($_ == 2 ? 50 : $_) . ')')
		  for (1 .. $n);
		$node->stop('immediate');
		$s->{run}->kill_kill;
		$node->start;
		ok( $node->poll_query_until(
				'postgres',
				"SELECT count(*) = 10 FROM $t WHERE v = 'orig' || id"),
			"$am: $n updates, crash: committed rows restored");
	}
}

$node->stop;
done_testing();
