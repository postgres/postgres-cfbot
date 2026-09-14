# Copyright (c) 2024-2026, PostgreSQL Global Development Group

# The logical replication apply worker locks the row it is about to update or
# delete (RelationFindReplTupleByIndex, table_tuple_lock without
# TUPLE_LOCK_FLAG_FIND_LAST_VERSION) and, with a BEFORE ROW trigger enabled
# for replication, locks it again from GetTupleForTrigger, under an
# EvalPlanQual state that has no plan.  Run the same changes against
# concurrent local transactions on the subscriber, once with heap on both
# sides and once with FLUX on both sides, and require FLUX to end in heap's
# state.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $schema = q{
	CREATE TABLE t (id int PRIMARY KEY, v int, w int) USING %AM%;
	CREATE TABLE tt (id int PRIMARY KEY, v int, w int) USING %AM%;
};
my $sub_extra = q{
	CREATE FUNCTION tt_bu() RETURNS trigger LANGUAGE plpgsql AS $$
	BEGIN
		IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
		NEW.w := NEW.w + 100;
		RETURN NEW;
	END $$;
	CREATE TRIGGER tt_bu BEFORE UPDATE OR DELETE ON tt
		FOR EACH ROW EXECUTE FUNCTION tt_bu();
	ALTER TABLE tt ENABLE ALWAYS TRIGGER tt_bu;
};

# Wait until the apply worker is blocked on a lock; report whether it was.
sub apply_waits
{
	my ($node) = @_;
	for (1 .. 50)
	{
		return 'waited'
		  if $node->safe_psql(
			'postgres',
			q{SELECT count(*) FROM pg_stat_activity
			  WHERE backend_type = 'logical replication apply worker'
			  AND wait_event_type = 'Lock'}) > 0;
		select(undef, undef, undef, 0.1);
	}
	return 'no wait';
}

sub run_am
{
	my ($am) = @_;
	my %r;

	my $pub = PostgreSQL::Test::Cluster->new("pub_$am");
	$pub->init(allows_streaming => 'logical');
	$pub->start;
	my $sub = PostgreSQL::Test::Cluster->new("sub_$am");
	$sub->init;
	$sub->start;

	(my $ddl = $schema) =~ s/%AM%/$am/g;
	$pub->safe_psql('postgres', $ddl);
	$sub->safe_psql('postgres', $ddl . $sub_extra);
	$pub->safe_psql(
		'postgres',
		q{INSERT INTO t SELECT g, g, 0 FROM generate_series(1, 8) g;
		  INSERT INTO tt SELECT g, g, 0 FROM generate_series(1, 8) g;
		  CREATE PUBLICATION p FOR TABLE t, tt});
	my $connstr = $pub->connstr . ' dbname=postgres';
	$sub->safe_psql('postgres',
		"CREATE SUBSCRIPTION s CONNECTION '$connstr' PUBLICATION p");
	$sub->wait_for_subscription_sync($pub, 's');

	my $q =
	  'SELECT string_agg(format(\'(%s,%s,%s)\', id, v, w), \' \' ORDER BY id) FROM ';

	# A replicated change against an in-progress local transaction that
	# updates or deletes the same row: the apply worker waits for it, then
	# applies the change to the committed version, if one is left.
	my @cases = (
		[
			'update vs local update',
			't', 'UPDATE t SET w = w + 1 WHERE id = 1',
			'COMMIT', 'UPDATE t SET v = v + 100 WHERE id = 1'
		],
		[
			'update vs local rollback',
			't',
			'UPDATE t SET w = w + 1 WHERE id = 2',
			'ROLLBACK',
			'UPDATE t SET v = v + 100 WHERE id = 2'
		],
		[
			'delete vs local update',
			't', 'UPDATE t SET w = w + 1 WHERE id = 3',
			'COMMIT', 'DELETE FROM t WHERE id = 3'
		],
		[
			'update vs local lock + update',
			't',
			'SELECT * FROM t WHERE id = 5 FOR UPDATE; UPDATE t SET w = w + 1 WHERE id = 5',
			'COMMIT',
			'UPDATE t SET v = v + 100 WHERE id = 5'
		],
		[
			'trigger: update, no conflict', 'tt',
			undef, undef,
			'UPDATE tt SET v = v + 100 WHERE id = 1'
		],
		[
			'trigger: update vs local update',
			'tt',
			'UPDATE tt SET w = w + 1 WHERE id = 2',
			'COMMIT',
			'UPDATE tt SET v = v + 100 WHERE id = 2'
		],
		[
			'trigger: update vs local update twice',
			'tt',
			'UPDATE tt SET w = w + 1 WHERE id = 3; UPDATE tt SET w = w + 1 WHERE id = 3',
			'COMMIT',
			'UPDATE tt SET v = v + 100 WHERE id = 3'
		],
		[
			'trigger: delete vs local update',
			'tt',
			'UPDATE tt SET w = w + 1 WHERE id = 4',
			'COMMIT',
			'DELETE FROM tt WHERE id = 4'
		],
		[
			'update vs local delete',
			't', 'DELETE FROM t WHERE id = 6',
			'COMMIT', 'UPDATE t SET v = v + 100 WHERE id = 6'
		],
		[
			'trigger: delete, no conflict', 'tt',
			undef, undef,
			'DELETE FROM tt WHERE id = 6'
		],
		[
			'trigger: update vs local delete',
			'tt', 'DELETE FROM tt WHERE id = 7',
			'COMMIT', 'UPDATE tt SET v = v + 100 WHERE id = 7'
		],);

	foreach my $c (@cases)
	{
		my ($name, $tab, $local, $end, $remote) = @$c;
		my $wait = '';
		if (defined $local)
		{
			my $bg = $sub->background_psql('postgres');
			$bg->query_safe("BEGIN; $local");
			$pub->safe_psql('postgres', $remote);
			$wait = apply_waits($sub);
			$bg->query_safe($end);
			$bg->quit;
		}
		else
		{
			$pub->safe_psql('postgres', $remote);
		}
		$pub->wait_for_catchup('s');
		$r{$name} = "$wait | " . $sub->safe_psql('postgres', $q . $tab);
	}

	# The replicated columns agree with the publisher, except on the rows
	# a local transaction deleted (the publisher's change to them found
	# nothing to apply).
	foreach my $tk ([ 't', '6' ], [ 'tt', '7' ])
	{
		my ($tab, $skip) = @$tk;
		my $rq =
			"SELECT string_agg(format('(%s,%s)', id, v), ' ' ORDER BY id) "
		  . "FROM $tab WHERE id NOT IN ($skip)";
		$r{"$tab matches publisher"} =
		  $sub->safe_psql('postgres', $rq) eq $pub->safe_psql('postgres', $rq)
		  ? 'yes'
		  : 'no: '
		  . $sub->safe_psql('postgres', $rq) . ' vs '
		  . $pub->safe_psql('postgres', $rq);
	}

	# Nothing crashed, and the worker that applied all this is still the
	# first one.
	$r{'server crashes'} = () =
	  slurp_file($sub->logfile) =~ /terminated by signal/g;
	$r{'apply errors'} = () = slurp_file($sub->logfile) =~ /ERROR:/g;

	$sub->safe_psql('postgres', 'DROP SUBSCRIPTION s');
	$sub->stop;
	$pub->stop;
	return \%r;
}

my $heap = run_am('heap');
my $flux = run_am('flux');

foreach my $k (sort keys %$heap)
{
	is($flux->{$k}, $heap->{$k}, "flux matches heap: $k");
	note "heap $k: $heap->{$k}";
}
foreach my $am ([ 'heap', $heap ], [ 'flux', $flux ])
{
	my ($name, $r) = @$am;
	is($r->{'server crashes'},
		0, "$name: no \"terminated by signal\" in the subscriber log");
	is($r->{'t matches publisher'}, 'yes', "$name: t matches the publisher");
	is($r->{'tt matches publisher'}, 'yes',
		"$name: tt matches the publisher");
}

done_testing();
