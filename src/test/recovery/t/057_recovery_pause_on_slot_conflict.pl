# Copyright (c) 2026, PostgreSQL Global Development Group

# Test recovery_pause_on_logical_slot_conflict with an archive-fed standby.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(time usleep);

sub drain_slot
{
	my ($node) = @_;
	my $decoded = 0;
	my $last_catalog_xmin;

	# A decoding pass can confirm one xmin candidate and expose the next one.
	# Continue until a complete pass produces neither output nor a new horizon.
	for (1 .. 100)
	{
		my $count = $node->safe_psql('postgres', qq[
SELECT count(*)
FROM pg_logical_slot_get_changes('archive_slot', NULL, NULL)
]);
		my $catalog_xmin = $node->safe_psql('postgres', qq[
SELECT catalog_xmin::text
FROM pg_replication_slots
WHERE slot_name = 'archive_slot'
]);

		$decoded += $count;
		return $decoded
		  if $count == 0
		  && defined($last_catalog_xmin)
		  && $catalog_xmin eq $last_catalog_xmin;

		$last_catalog_xmin = $catalog_xmin;
	}

	die "logical slot did not finish advancing its catalog_xmin";
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 'logical', has_archiving => 1);
$primary->append_conf('postgresql.conf', qq[
wal_level = logical
autovacuum = off
]);
$primary->start;

$primary->safe_psql('postgres', qq[
CREATE TABLE events (id integer PRIMARY KEY, payload text);
ALTER TABLE events REPLICA IDENTITY FULL;
INSERT INTO events VALUES (0, 'seed');
]);

my $backup_name = 'backup';
$primary->backup($backup_name);

# Supply post-backup WAL before starting the archive-fed standby.
$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
my $initial_lsn = $primary->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
my $initial_seg = $primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$initial_lsn')");
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->poll_query_until('postgres', qq[
SELECT last_archived_wal >= '$initial_seg' FROM pg_stat_archiver
]) or die "timed out waiting for initial WAL segment to be archived";

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $backup_name,
	has_streaming => 0, has_restoring => 1);
$standby->append_conf('postgresql.conf', qq[
hot_standby = on
recovery_pause_on_logical_slot_conflict = on
max_standby_archive_delay = -1
]);
$standby->start;
$standby->poll_query_until('postgres',
	"SELECT pg_last_wal_replay_lsn() >= '$initial_lsn'")
  or die "standby did not replay the initial WAL";

# Slot creation needs a future running-xacts record to reach consistency.
# Start it first, then archive such a record, rather than depending on timing
# between startup replay and the slot-creation query.
my $slot_session = $standby->background_psql('postgres');
$slot_session->query_until(qr/slot creation started/, qq[
\\echo slot creation started
SELECT pg_create_logical_replication_slot('archive_slot', 'test_decoding');
]);

$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
my $slot_lsn = $primary->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
my $slot_seg = $primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$slot_lsn')");
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->poll_query_until('postgres', qq[
SELECT last_archived_wal >= '$slot_seg' FROM pg_stat_archiver
]) or die "timed out waiting for slot-creation WAL to be archived";
$slot_session->quit;

my $initial_catalog_xmin = $standby->safe_psql('postgres', qq[
SELECT catalog_xmin::text
FROM pg_replication_slots
WHERE slot_name = 'archive_slot'
]);
ok($initial_catalog_xmin ne '', 'logical slot reached consistency');

# Put a newer running-xacts record before the catalog cleanup.  Consuming up
# to the pause can therefore give logical decoding a safe, newer catalog_xmin.
$primary->safe_psql('postgres', qq[
INSERT INTO events
SELECT g, 'payload-' || g FROM generate_series(1, 3000) AS g;
]);
for my $i (1 .. 20)
{
	$primary->safe_psql('postgres',
		"CREATE TABLE churn_$i (a int); DROP TABLE churn_$i");
	$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
	$primary->safe_psql('postgres',
		"INSERT INTO events VALUES (10000 + $i, 'advance xid')");
}
$primary->safe_psql('postgres', 'ANALYZE events');
$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
$primary->safe_psql('postgres',
	"INSERT INTO events VALUES (20001, 'advance xid')");
$primary->safe_psql('postgres', 'ANALYZE events');
$primary->safe_psql('postgres', 'SELECT pg_log_standby_snapshot()');
$primary->safe_psql('postgres',
	"INSERT INTO events VALUES (20002, 'advance xid')");
$primary->safe_psql('postgres', qq[
VACUUM pg_class;
VACUUM pg_attribute;
VACUUM pg_type;
VACUUM pg_depend;
VACUUM pg_statistic;
]);

my $target_lsn = $primary->safe_psql('postgres',
	'SELECT pg_current_wal_insert_lsn()');
my $target_seg = $primary->safe_psql('postgres',
	"SELECT pg_walfile_name('$target_lsn')");
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->poll_query_until('postgres', qq[
SELECT last_archived_wal >= '$target_seg' FROM pg_stat_archiver
]) or die "timed out waiting for conflict WAL to be archived";

my $decoded = 0;
my $pauses = 0;
my $reached_target = 0;
my $deadline = time() + 60;
my %handled_pause;
my $manual_pause_tested = 0;

while (time() < $deadline)
{
	my $state = $standby->safe_psql('postgres',
		'SELECT pg_get_wal_replay_pause_state()');

	if ($state eq 'paused' || $state eq 'pause requested')
	{
		my $pause_lsn = $standby->safe_psql('postgres',
			'SELECT pg_last_wal_replay_lsn()');

		if (!$handled_pause{$pause_lsn}++)
		{
			my $preserve_manual_pause = !$manual_pause_tested;
			my $log_offset;

			if ($preserve_manual_pause)
			{
				$standby->safe_psql('postgres',
					'SELECT pg_wal_replay_pause()');
				$log_offset = -s $standby->logfile;
			}

			$decoded += drain_slot($standby);
			$pauses++;

			if ($preserve_manual_pause)
			{
				$standby->wait_for_log(
					qr/recovery remains paused due to a separate pause request/,
					$log_offset);
				is($standby->safe_psql('postgres',
						'SELECT pg_get_wal_replay_pause_state()'),
					'paused', 'auto-resume preserved a concurrent manual pause');
				$standby->safe_psql('postgres',
					'SELECT pg_wal_replay_resume()');
				$manual_pause_tested = 1;
			}
		}
	}

	$reached_target = $standby->safe_psql('postgres',
		"SELECT pg_last_wal_replay_lsn() >= '$target_lsn'") eq 't';
	last if $reached_target;
	usleep(100_000);
}

ok($reached_target, 'standby replayed past the catalog cleanup');
cmp_ok($pauses, '>=', 1, 'recovery paused for a logical slot conflict');
cmp_ok($decoded, '>=', 3000, 'decoded changes while recovery was paused');

my ($invalidation_reason, $final_catalog_xmin) = split /\|/,
  $standby->safe_psql('postgres', qq[
SELECT coalesce(invalidation_reason, 'valid') || '|' || catalog_xmin::text
FROM pg_replication_slots
WHERE slot_name = 'archive_slot'
]);
is($invalidation_reason, 'valid', 'logical slot was not invalidated');
cmp_ok($final_catalog_xmin, '>', $initial_catalog_xmin,
	'logical decoding advanced catalog_xmin');

# A shared-catalog conflict applies to logical slots in every database.  A
# promotion request must release that pause without requiring a separate
# pg_wal_replay_resume() call.
$primary->safe_psql('postgres', qq[
INSERT INTO events VALUES (3001, 'more WAL');
CREATE ROLE archive_pause_test;
DROP ROLE archive_pause_test;
SELECT pg_log_standby_snapshot();
INSERT INTO events VALUES (3002, 'advance xid');
VACUUM pg_authid;
]);
my $promote_seg = $primary->safe_psql('postgres',
	'SELECT pg_walfile_name(pg_current_wal_insert_lsn())');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->poll_query_until('postgres', qq[
SELECT last_archived_wal >= '$promote_seg' FROM pg_stat_archiver
]) or die "timed out waiting for promotion-test WAL to be archived";

$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "recovery did not pause before the promotion test";

my $started = time();
$standby->promote;
is($standby->safe_psql('postgres', 'SELECT pg_is_in_recovery()'), 'f',
	'standby promoted while conflict pause was active');
cmp_ok(time() - $started, '<', 10, 'promotion released the conflict pause');

$standby->stop;
$primary->stop;

done_testing();
