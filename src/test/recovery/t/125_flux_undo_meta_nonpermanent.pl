# Copyright (c) 2026, PostgreSQL Global Development Group

# Undo of a temporary, unlogged or WAL-skipping relation goes to an undo log
# that recovery never replays.  The undo-log meta-data record
# (XLOG_UNDOLOG_META) that maps a transaction to its undo log must therefore
# only ever be written for the permanent undo log: if it named the
# non-permanent log, replay would map the transaction to that log and the
# replay of the same transaction's later permanent undo would fail.
#
# Each case runs in one transaction: a change to a permanent table (attaches
# the permanent undo log), a checkpoint from another session (so the next undo
# insert is the first after the checkpoint), a change to the non-permanent
# relation, and another change to the permanent table.  Crash recovery, and
# for the temporary and unlogged cases a streaming standby, must replay that.
# Heap is the control.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

# Wait until $standby replayed up to $lsn or logged a PANIC.
sub standby_replays
{
	my ($standby, $lsn) = @_;

	foreach (1 .. $PostgreSQL::Test::Utils::timeout_default)
	{
		return 0 if slurp_file($standby->logfile) =~ /PANIC/;
		my ($ret, $out) = $standby->psql('postgres',
			"SELECT pg_last_wal_replay_lsn() >= '$lsn'::pg_lsn");
		return 1 if $ret == 0 && $out eq 't';
		sleep 1;
	}
	return 0;
}

sub run_case
{
	my ($node, $standby, $am, $kind) = @_;
	my $s = $node->background_psql('postgres');
	my $nonperm;

	if ($kind eq 'temp')
	{
		$s->query_safe(
			"CREATE TEMP TABLE np (id int, v int) USING $am; INSERT INTO np VALUES (1, 0)"
		);
		$nonperm = 'UPDATE np SET v = v + 1';
	}
	elsif ($kind eq 'unlogged')
	{
		$node->safe_psql('postgres',
			"CREATE UNLOGGED TABLE np (id int, v int) USING $am; INSERT INTO np VALUES (1, 0)"
		);
		$nonperm = 'UPDATE np SET v = v + 1';
	}
	else
	{
		$nonperm =
		  "CREATE TABLE np (id int, v int) USING $am; INSERT INTO np VALUES (1, 0); UPDATE np SET v = v + 1";
	}

	$s->query_safe('BEGIN');
	$s->query_safe('UPDATE perm SET v = v + 1 WHERE id = 1');
	$node->safe_psql('postgres', 'CHECKPOINT');
	$s->query_safe($nonperm);
	$s->query_safe('UPDATE perm SET v = v + 1 WHERE id = 2');
	$s->query_safe('COMMIT');
	$s->quit;

	if ($standby)
	{
		my $ok = ok(
			standby_replays($standby, $node->lsn('insert')),
			"$am $kind: standby replays the transaction");
		if (!$ok)
		{
			my ($panic) = slurp_file($standby->logfile) =~ /(PANIC:.*)/;
			diag($panic // 'no PANIC');
		}
		else
		{
			is($standby->safe_psql('postgres', 'SELECT sum(v) FROM perm'),
				'2', "$am $kind: standby sees the permanent changes");
		}
	}

	$node->stop('immediate');
	my $ret = $node->start(fail_ok => 1);
	ok($ret, "$am $kind: crash recovery replays the transaction");
	if (!$ret)
	{
		my ($panic) = slurp_file($node->logfile) =~ /(PANIC:.*)/;
		diag($panic // 'no PANIC');
		return;
	}
	is($node->safe_psql('postgres', 'SELECT sum(v) FROM perm'),
		'2', "$am $kind: permanent changes survive the crash");
}

for my $am (qw(heap flux))
{
	for my $kind (qw(temp unlogged skip-WAL))
	{
		my $node = PostgreSQL::Test::Cluster->new("${am}_$kind");
		my $standby;

		if ($kind eq 'skip-WAL')
		{
			# A permanent relation created in the same transaction skips WAL.
			$node->init;
			$node->append_conf(
				'postgresql.conf', qq[
wal_level = minimal
max_wal_senders = 0
wal_skip_threshold = 0
]);
		}
		else
		{
			$node->init(allows_streaming => 1);
		}
		$node->append_conf('postgresql.conf', 'autovacuum = off');
		$node->start;
		$node->safe_psql('postgres',
			"CREATE TABLE perm (id int PRIMARY KEY, v int) USING $am; INSERT INTO perm SELECT g, 0 FROM generate_series(1, 10) g"
		);
		if ($kind ne 'skip-WAL')
		{
			$node->backup('bkp');
			$standby =
			  PostgreSQL::Test::Cluster->new("${am}_${kind}_standby");
			$standby->init_from_backup($node, 'bkp', has_streaming => 1);
			$standby->start;
		}
		run_case($node, $standby, $am, $kind);
		$standby->stop('immediate', fail_ok => 1) if $standby;
		$node->stop('immediate', fail_ok => 1);
	}
}

done_testing();
