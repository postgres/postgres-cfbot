# Copyright (c) 2026, PostgreSQL Global Development Group

# Check that pg_basebackup's replication slot reserves WAL from before the
# backup's start position.  If the slot were created only after BASE_BACKUP
# had returned the start position, a checkpoint running in between could
# remove the WAL the backup needs, and streaming it would fail.
#
# We hold the slot creation with an injection point, force WAL past several
# segments and checkpoint, then let the slot creation continue.

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

done_testing();
