# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;
$primary->backup('backup');

my @counters = qw(prefetch hit skip_init skip_new skip_fpw skip_rep);
my $counter_query =
  'SELECT ' . join(', ', @counters) . ' FROM pg_stat_recovery_prefetch';

for my $mode ('off', 'try')
{
	my $standby = PostgreSQL::Test::Cluster->new("standby_$mode");
	$standby->init_from_backup($primary, 'backup', has_streaming => 1);
	$standby->append_conf(
		'postgresql.conf',
		"recovery_prefetch = '$mode'\nmaintenance_io_concurrency = 10");
	$standby->start;

	$primary->safe_psql('postgres',
		"CREATE TABLE tab_$mode AS SELECT generate_series(1, 10000) AS a");
	$primary->wait_for_replay_catchup($standby);

	my @values = split /\|/, $standby->safe_psql('postgres', $counter_query);
	if ($mode eq 'off')
	{
		is(join('|', @values), '0|0|0|0|0|0',
			'prefetch counters are zero when disabled throughout recovery');
	}
	else
	{
		# Retain counters collected before prefetching was disabled.
		$standby->safe_psql('postgres',
			"ALTER SYSTEM SET recovery_prefetch = 'off'");
		$standby->reload;
		ok( $standby->poll_query_until(
				'postgres', "SHOW recovery_prefetch", 'off'),
			'prefetching is disabled before promotion');
		$primary->safe_psql('postgres', "INSERT INTO tab_try VALUES (10001)");
		$primary->wait_for_replay_catchup($standby);
		is($standby->safe_psql('postgres', $counter_query),
			join('|', @values),
			'disabling prefetching preserves accumulated counters');
	}

	my $offset = -s $standby->logfile;
	$standby->promote;
	$standby->log_check(
		"prefetch summary after recovery with initial setting $mode",
		$offset,
		log_like => [
			qr/LOG:  redo prefetch stats: prefetched $values[0] blocks, skipped \($values[1] in the buffer pool, $values[2] zero-initialized, $values[3] non-existent, $values[4] full page image, $values[5] recently prefetched\)/,
			qr/DETAIL:  recovery_prefetch = off at the end of recovery\./
		]);
	my $log = slurp_file($standby->logfile, $offset);
	is(scalar(() = $log =~ /LOG:  redo prefetch stats:/g),
		1, 'one prefetch summary is logged at recovery completion');
	$standby->stop;
}

$primary->stop;
done_testing();
