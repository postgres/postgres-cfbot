# Copyright (c) 2026, PostgreSQL Global Development Group
#
# A standby stopped at a recovery target receives WAL past the target that
# truncates a table.  A standby only summarizes replayed WAL, so an
# incremental backup taken after promotion still includes blocks modified
# before the target.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', <<EOF);
summarize_wal = on
autovacuum = off
EOF
$primary->start;

# One row per block.
my $pad_size = $primary->safe_psql('postgres', 'SHOW block_size') / 2;
$primary->safe_psql('postgres', <<EOF);
CREATE TABLE t (id int, v int, pad text STORAGE PLAIN);
INSERT INTO t SELECT g, 0, repeat('x', $pad_size) FROM generate_series(1, 4) g;
VACUUM t;
EOF

$primary->backup('full');
my $full_path = $primary->backup_dir . '/full';

my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'full', has_streaming => 1);
$standby->append_conf('postgresql.conf', <<EOF);
summarize_wal = on
recovery_target_action = 'pause'
EOF

# Modify the last block, and stop recovery right after that.
$primary->safe_psql('postgres', 'UPDATE t SET v = 1 WHERE id = 4');
my $target =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');
$standby->append_conf('postgresql.conf', "recovery_target_lsn = '$target'\n");
$standby->start;

# Truncate the table on the old timeline only.
$primary->safe_psql('postgres', 'DELETE FROM t WHERE id > 1');
$primary->safe_psql('postgres', 'VACUUM t');
$primary->safe_psql('postgres', 'SELECT pg_switch_wal()');
$primary->safe_psql('postgres', 'CHECKPOINT');
my $end_lsn =
  $primary->safe_psql('postgres', 'SELECT pg_current_wal_insert_lsn()');

$standby->poll_query_until('postgres',
	"SELECT pg_get_wal_replay_pause_state() = 'paused'")
  or die "standby did not pause at the target";

# Wait until the standby has received the truncation and the summarizer has
# caught up with replay.
$standby->poll_query_until('postgres',
	"SELECT pg_last_wal_receive_lsn() >= '$end_lsn'")
  or die "standby did not receive up to $end_lsn";
$standby->poll_query_until('postgres',
	"SELECT pending_lsn >= pg_last_wal_replay_lsn() FROM pg_get_wal_summarizer_state()"
) or die "summarizer did not catch up with replay";

$primary->stop;
$standby->safe_psql('postgres', 'SELECT pg_promote()');
$standby->poll_query_until('postgres', 'SELECT NOT pg_is_in_recovery()')
  or die "standby was not promoted";

# Back up before reading t, so that setting hint bits can't hide the problem.
$standby->command_ok(
	[
		'pg_basebackup', '--no-sync',
		'--pgdata' => $primary->backup_dir . '/incr',
		'--checkpoint' => 'fast',
		'--incremental' => "$full_path/backup_manifest",
	],
	'incremental backup after promotion');
$standby->stop;

my $restored = PostgreSQL::Test::Cluster->new('restored');
$restored->init_from_backup($primary, 'incr', combine_with_prior => ['full']);
$restored->start;
is($restored->safe_psql('postgres', 'SELECT id, v FROM t ORDER BY id'),
	"1|0\n2|0\n3|0\n4|1", 'restored backup has the update');
$restored->stop;

done_testing();
