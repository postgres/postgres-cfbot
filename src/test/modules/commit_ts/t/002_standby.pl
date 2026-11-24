
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Test simple scenario involving a standby

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Utils;
use Test::More;
use PostgreSQL::Test::Cluster;

my $bkplabel = 'backup';
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);

$primary->append_conf(
	'postgresql.conf', qq{
	track_commit_timestamp = on
	max_wal_senders = 5
	});
$primary->start;
$primary->backup($bkplabel);

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, $bkplabel, has_streaming => 1);
$standby->append_conf('postgresql.conf', 'commit_timestamp_buffers = 16');
$standby->start;

for my $i (1 .. 10)
{
	$primary->safe_psql('postgres', "create table t$i()");
}
my $primary_ts = $primary->safe_psql('postgres',
	qq{SELECT ts.* FROM pg_class, pg_xact_commit_timestamp(xmin) AS ts WHERE relname = 't10'}
);
my $primary_lsn =
  $primary->safe_psql('postgres', 'select pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
	qq{SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()})
  or die "standby never caught up";

my $standby_ts = $standby->safe_psql('postgres',
	qq{select ts.* from pg_class, pg_xact_commit_timestamp(xmin) ts where relname = 't10'}
);
is($primary_ts, $standby_ts, "standby gives same value as primary");

my $before_commits = $primary->safe_psql('postgres',
	"SELECT pg_create_restore_point('before_commits')");

# Fill more than 16 pages of timestamps (10 bytes per XID) to force eviction.
my $after_commits = $primary->safe_psql(
	'postgres', q{
	DO $$
	BEGIN
		FOR counter IN 1..17 * (current_setting('block_size')::int / 10) LOOP
			PERFORM pg_current_xact_id();
			COMMIT;
		END LOOP;
	END $$;
	SELECT pg_create_restore_point('after_commits');
});
$primary->wait_for_catchup($standby, 'replay', $after_commits);
is( $standby->safe_psql(
		'postgres',
		"SELECT min_recovery_end_lsn > '$before_commits' FROM pg_control_recovery()"
	),
	't',
	'writing commit timestamps advances the minimum recovery point');

$primary->append_conf('postgresql.conf', 'track_commit_timestamp = off');
$primary->restart;
$primary->safe_psql('postgres', 'checkpoint');
$primary_lsn = $primary->safe_psql('postgres', 'select pg_current_wal_lsn()');
$standby->poll_query_until('postgres',
	qq{SELECT '$primary_lsn'::pg_lsn <= pg_last_wal_replay_lsn()})
  or die "standby never caught up";
$standby->safe_psql('postgres', 'checkpoint');

# This one should raise an error now
my ($ret, $standby_ts_stdout, $standby_ts_stderr) = $standby->psql('postgres',
	'select ts.* from pg_class, pg_xact_commit_timestamp(xmin) ts where relname = \'t10\''
);
is($ret, 3, 'standby errors when primary turned feature off');
is($standby_ts_stdout, '',
	"standby gives no value when primary turned feature off");
like(
	$standby_ts_stderr,
	qr/could not get commit timestamp data/,
	'expected error when primary turned feature off');

done_testing();
