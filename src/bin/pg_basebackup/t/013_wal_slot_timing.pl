# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that pg_basebackup's replication slot reserves WAL from before the
# backup's start position.  If the slot were created only after BASE_BACKUP
# had returned the start position, a checkpoint running in between could
# remove the WAL the backup needs, and streaming it would fail.
#
# We hold the slot creation with an injection point, force WAL past several
# segments and checkpoint, then let the slot creation continue.
#
# Also check when the WAL streaming connection is opened, that it is kept
# open while it waits for the backup's checkpoint, and that pg_basebackup
# reconnects if that checkpoint terminates the connection holding the
# temporary slot.

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

# With a temporary slot, the WAL streaming connection is opened before
# BASE_BACKUP and is idle until the backup's checkpoint has completed.
# pg_basebackup disables idle_session_timeout on it, so it must not be closed
# while it waits, even if the checkpoint takes longer than the timeout.  Hold
# the checkpoint until the connection has been idle for several times the
# timeout.  If it were closed, this would wait until the test times out.
#
# The options given in the connection string must still be used on that
# connection.  log_replication_commands is enabled only through them, so the
# temporary slot's creation is logged only if they were kept.
#
# Without a temporary slot, with --no-slot or with --slot naming an existing
# slot, the connection must be opened only after BASE_BACKUP has returned, so
# while the checkpoint is held, the only walsender is the one running
# BASE_BACKUP.
$node->append_conf(
	'postgresql.conf', q{
idle_session_timeout = '1s'
log_replication_commands = off
});
$node->reload;
$node->safe_psql('postgres',
	"SELECT pg_create_physical_replication_slot('existing_slot', true)");

my $connstr_with_options =
  $node->connstr('postgres') . " options='-c log_replication_commands=on'";

foreach my $case (
	[ 'temporary slot', 'backup_session_temp', 1, [] ],
	[ '--no-slot', 'backup_session_noslot', 0, ['--no-slot'] ],
	[
		'--slot', 'backup_session_existing',
		0, [ '--slot' => 'existing_slot' ]
	])
{
	my ($desc, $dirname, $early, $slot_opts) = @$case;
	my $dir = "$tempdir/$dirname";

	$node->safe_psql('postgres',
		"SELECT injection_points_attach('checkpoint-before-old-wal-removal', 'wait')"
	);

	my $log_offset = -s $node->logfile;
	my ($s_stdout, $s_stderr);
	my $s_h = IPC::Run::start(
		[
			'pg_basebackup', '--no-sync',
			'--checkpoint' => 'fast',
			'--pgdata' => $dir,
			'--wal-method' => 'stream',
			@$slot_opts,
			'--dbname' => $connstr_with_options,
		],
		'>' => \$s_stdout,
		'2>' => \$s_stderr);

	$node->wait_for_event('checkpointer',
		'checkpoint-before-old-wal-removal');
	if ($early)
	{
		$node->poll_query_until(
			'postgres', q{
			SELECT count(*) > 0 FROM pg_stat_activity
			WHERE backend_type = 'walsender' AND state = 'idle'
			  AND now() - state_change > interval '3s'})
		  or die
		  "timed out waiting for the WAL streaming connection to stay idle";
	}
	else
	{
		is( $node->safe_psql(
				'postgres', q{
				SELECT count(*) FROM pg_stat_activity
				WHERE backend_type = 'walsender'}),
			'1',
			"no WAL streaming connection during the checkpoint with $desc");
	}

	$node->safe_psql('postgres',
		"SELECT injection_points_detach('checkpoint-before-old-wal-removal')"
	);
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('checkpoint-before-old-wal-removal')"
	);

	ok($s_h->finish, "backup with $desc and idle_session_timeout")
	  or diag("pg_basebackup stderr:\n$s_stderr");
	unlike(
		$s_stderr,
		qr/connection for streaming write-ahead log was lost/,
		"WAL streaming connection with $desc was not lost");

	if ($early)
	{
		ok( $node->log_contains(
				qr/received replication command: CREATE_REPLICATION_SLOT "pg_basebackup_\d+" TEMPORARY/,
				$log_offset),
			"connection options are used for the WAL streaming connection with $desc"
		);
	}

	$node->command_ok([ 'pg_verifybackup', $dir ],
		"backup with $desc and idle_session_timeout verifies");
}

$node->safe_psql('postgres',
	"SELECT pg_drop_replication_slot('existing_slot')");
$node->append_conf(
	'postgresql.conf', q{
idle_session_timeout = 0
log_replication_commands = on
});
$node->reload;

# The temporary slot is created before the backup's checkpoint, so it starts
# at the previous one.  If more WAL than max_slot_wal_keep_size was written
# since then, the backup's checkpoint terminates the connection holding the
# slot, which drops it.  pg_basebackup must then reconnect and still get all
# the WAL the backup needs.
my $knode = PostgreSQL::Test::Cluster->new('keep');
$knode->init(allows_streaming => 1, extra => ['--wal-segsize=1']);
$knode->append_conf(
	'postgresql.conf', q{
wal_keep_size = 0
max_slot_wal_keep_size = 2MB
checkpoint_timeout = 1h
});
$knode->start;

foreach my $case (
	[ 'temporary slot', 'backup_keep_temp', [] ],
	[
		'--create-slot', 'backup_keep_created',
		[ '--create-slot', '--slot' => 'keep_slot' ]
	])
{
	my ($desc, $dirname, $slot_opts) = @$case;
	my $dir = "$tempdir/$dirname";

	$knode->safe_psql('postgres', 'CHECKPOINT');
	$knode->advance_wal(5);

	my $log_offset = -s $knode->logfile;
	$knode->command_checks_all(
		[
			'pg_basebackup', '--no-sync',
			'--checkpoint' => 'fast',
			'--pgdata' => $dir,
			'--wal-method' => 'stream',
			@$slot_opts,
			'--dbname' => $knode->connstr('postgres'),
		],
		0,
		[],
		[
			qr/connection for streaming write-ahead log was lost during the base backup checkpoint, reconnecting/
		],
		"backup with $desc and max_slot_wal_keep_size");

	ok( $knode->log_contains(
			qr/terminating process \d+ to release replication slot "pg_basebackup_\d+"/,
			$log_offset),
		"temporary slot's connection was terminated in backup with $desc");

	$knode->command_ok([ 'pg_verifybackup', $dir ],
		"backup with $desc and max_slot_wal_keep_size verifies");
}

is( $knode->safe_psql(
		'postgres',
		q{SELECT restart_lsn IS NOT NULL AND invalidation_reason IS NULL
			FROM pg_replication_slots WHERE slot_name = 'keep_slot'}),
	't',
	'slot created by --create-slot is valid after the backup');
$knode->safe_psql('postgres', "SELECT pg_drop_replication_slot('keep_slot')");

done_testing();
