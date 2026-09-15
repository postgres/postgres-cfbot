# Copyright (c) 2026, PostgreSQL Global Development Group

# Verify that pg_basebackup reserves WAL before requesting its startpoint.
#
# Hold BASE_BACKUP before sending the startpoint, generate WAL, and checkpoint.
# The early-created slot must retain the start segment until streaming starts.

use strict;
use warnings FATAL => 'all';
use File::Path qw(rmtree);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($ENV{enable_injection_points} ne 'yes')
{
	plan skip_all => 'Injection points not supported by this build';
}

# Small WAL segments make recycling cheap.
my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1, extra => [ '--wal-segsize', '1' ]);
$node->append_conf(
	'postgresql.conf', q[
wal_keep_size = 0
min_wal_size = 2MB
max_wal_size = 4MB
checkpoint_timeout = 1h
]);
$node->start;

# injection_points may not be installed under installcheck.
if (!$node->check_extension('injection_points'))
{
	plan skip_all => 'Extension injection_points not installed';
}
$node->safe_psql('postgres', 'CREATE EXTENSION injection_points;');

for my $mode ('permanent', 'temporary')
{
	note "Testing $mode replication slot";

	# Stop BASE_BACKUP before it sends the selected startpoint to the client.
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('basebackup-before-send-startpoint', 'wait');"
	);

	my $backupdir = $node->backup_dir . '/basebackup_race_' . $mode;
	my ($bb_stdout, $bb_stderr) = ('', '');
	my $bb_timeout =
	  IPC::Run::timeout(3 * $PostgreSQL::Test::Utils::timeout_default);
	my $bb = IPC::Run::start(
		[
			'pg_basebackup',
			'--pgdata' => $backupdir,
			'--wal-method' => 'stream',
			(   $mode eq 'permanent'
				? ('--slot' => 'basebackup_race', '--create-slot')
				: ()),
			'--checkpoint' => 'fast',
			'--no-sync',
			'-d' => $node->connstr('postgres')
		],
		'>' => \$bb_stdout,
		'2>' => \$bb_stderr,
		$bb_timeout);

	$node->wait_for_event('walsender', 'basebackup-before-send-startpoint');

	# The slot must already reserve WAL before the client receives the startpoint.
	is( $node->safe_psql(
			'postgres',
			'SELECT count(*) FROM pg_replication_slots '
			  . 'WHERE restart_lsn IS NOT NULL;'),
		'1',
		'replication slot reserves WAL before receiving the startpoint');

	# do_pg_backup_start() used the current checkpoint's REDO pointer.
	my $startpoint_wal = $node->safe_psql('postgres',
		'SELECT pg_walfile_name(redo_lsn) FROM pg_control_checkpoint();');
	note "backup startpoint is in WAL segment $startpoint_wal";

	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_ls_waldir() WHERE name = '$startpoint_wal';"
		),
		'1',
		'WAL segment containing the backup startpoint exists while waiting');

	# Force enough WAL activity and a checkpoint to recycle the start segment
	# unless the replication slot protects it.
	$node->advance_wal(10);
	$node->safe_psql('postgres', 'CHECKPOINT;');
	is( $node->safe_psql(
			'postgres',
			"SELECT count(*) FROM pg_ls_waldir() WHERE name = '$startpoint_wal';"
		),
		'1',
		'WAL segment containing the backup startpoint survives a concurrent '
		  . 'checkpoint');

	# Let pg_basebackup request WAL from the retained startpoint.
	$node->safe_psql('postgres',
		"SELECT injection_points_wakeup('basebackup-before-send-startpoint');"
	);
	$node->safe_psql('postgres',
		"SELECT injection_points_detach('basebackup-before-send-startpoint');"
	);

	$bb->finish;
	note "pg_basebackup stderr:\n$bb_stderr";

	is($bb->result(0), 0,
		'pg_basebackup succeeded despite concurrent WAL recycling')
	  or diag
	  "pg_basebackup stdout: $bb_stdout\npg_basebackup stderr: $bb_stderr";

	rmtree($backupdir);
	if ($mode eq 'permanent')
	{
		$node->safe_psql('postgres',
			"SELECT pg_drop_replication_slot('basebackup_race');");
	}
	ok( $node->poll_query_until(
			'postgres', 'SELECT count(*) = 0 FROM pg_replication_slots;'),
		'replication slot cleaned up');
}

done_testing();
