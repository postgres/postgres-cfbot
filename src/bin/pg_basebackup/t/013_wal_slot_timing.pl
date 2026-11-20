# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that pg_basebackup's replication slot reserves WAL from before the
# backup's start position.  If the slot were created only after BASE_BACKUP
# had returned the start position, a checkpoint running in between could
# remove the WAL the backup needs, and streaming it would fail.
#
# We hold the slot creation with an injection point, force WAL past several
# segments and checkpoint, then let the slot creation continue.
#
# Also check that the slot, created before BASE_BACKUP, is already in use
# when the backup's checkpoint runs, so that the checkpoint cannot
# invalidate it under idle_replication_slot_timeout.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

my $tempdir = PostgreSQL::Test::Utils::tempdir;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
wal_keep_size = 0
checkpoint_timeout = 1h
idle_replication_slot_timeout = 1h
});
$node->start;

if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}

$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
$node->safe_psql('postgres',
	"SELECT injection_points_attach('replication-slot-create-begin', 'wait')"
);

my $backupdir = "$tempdir/backup";
my ($stdout, $stderr);
my $h = IPC::Run::start(
	[
		'pg_basebackup', '--no-sync',
		'--checkpoint' => 'fast',
		'--pgdata' => $backupdir,
		'--wal-method' => 'stream',
		'--dbname' => $node->connstr('postgres'),
	],
	'>' => \$stdout,
	'2>' => \$stderr);

# Wait until the walsender is about to create pg_basebackup's slot.
$node->wait_for_event('walsender', 'replication-slot-create-begin');

# Move WAL forward by several segments and checkpoint, so that every segment
# not protected by a slot or wal_keep_size is removed or recycled.
$node->advance_wal(3);
$node->safe_psql('postgres', 'CHECKPOINT');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('replication-slot-create-begin')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('replication-slot-create-begin')");

my $ok = $h->finish;
ok($ok, 'pg_basebackup succeeds with WAL activity during slot creation')
  or diag("pg_basebackup stderr:\n$stderr");
unlike(
	$stderr,
	qr/has already been removed/,
	'no WAL needed by the backup was removed');

# pg_verifybackup also parses the WAL the backup needs, so this fails if
# any of it is missing.
$node->command_ok([ 'pg_verifybackup', $backupdir ],
	'backup with streamed WAL verifies');

# Now the idle timeout.  idle_replication_slot_timeout is set, and with
# slot-timeout-inval attached, every checkpoint invalidates every slot that
# is idle at that moment, however briefly.  Hold the backup's checkpoint just
# before it checks the slots, wait until pg_basebackup's slot is being
# streamed from, and then let the checkpoint continue.  An idle slot created
# beforehand shows that the checkpoint does invalidate idle slots.
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('idle_slot', true)");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('slot-timeout-inval', 'error')");
$node->safe_psql('postgres',
	"SELECT injection_points_attach('checkpoint-before-old-wal-removal', 'wait')"
);

my $backupdir2 = "$tempdir/backup_idle";
$h = IPC::Run::start(
	[
		'pg_basebackup', '--no-sync',
		'--checkpoint' => 'fast',
		'--pgdata' => $backupdir2,
		'--wal-method' => 'stream',
		'--dbname' => $node->connstr('postgres'),
	],
	'>' => \$stdout,
	'2>' => \$stderr);

$node->wait_for_event('checkpointer', 'checkpoint-before-old-wal-removal');

# A slot that is being streamed from has no inactive_since.
my $in_use = $node->poll_query_until(
	'postgres', q{
	SELECT count(*) = 1 FROM pg_replication_slots
		WHERE slot_name LIKE 'pg\_basebackup\_%' AND inactive_since IS NULL
});
ok($in_use, "pg_basebackup's slot is in use during the backup's checkpoint");

# Detach before waking up, so that later checkpoints, such as the one at
# shutdown, do not wait.
$node->safe_psql('postgres',
	"SELECT injection_points_detach('checkpoint-before-old-wal-removal')");
$node->safe_psql('postgres',
	"SELECT injection_points_wakeup('checkpoint-before-old-wal-removal')");

$ok = $h->finish;
ok($ok, 'pg_basebackup succeeds when idle slots are invalidated')
  or diag("pg_basebackup stderr:\n$stderr");

is( $node->safe_psql(
		'postgres',
		q{SELECT invalidation_reason FROM pg_replication_slots
			WHERE slot_name = 'idle_slot'}),
	'idle_timeout',
	'idle slot was invalidated by the backup\'s checkpoint');

$node->safe_psql('postgres',
	"SELECT injection_points_detach('slot-timeout-inval')");

$node->command_ok([ 'pg_verifybackup', $backupdir2 ],
	'backup taken while idle slots are invalidated verifies');

done_testing();
