
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

#
# Test that running pg_rewind with the source and target clusters
# on the same timeline runs successfully.
#
use strict;
use warnings FATAL => 'all';
use File::Copy;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

use FindBin;
use lib $FindBin::RealBin;

use RewindTest;

RewindTest::setup_cluster();
RewindTest::start_primary();
RewindTest::create_standby();
RewindTest::run_pg_rewind('local');
RewindTest::clean_rewind_test();

# Helper function to run pg_rewind in local mode with the given source and
# target nodes and extra arguments.
#
# The target and source nodes are stopped before the call and the target is
# restarted afterward.  The target's postgresql.conf is copied to a temporary
# location and passed to pg_rewind with --config-file, so that pg_rewind can
# update the target's config file in place without worrying about file
# permissions.  The temporary config file is moved back to the target's data
# directory and permissions fixed after pg_rewind finishes.
#
# If the last element of @extra_args is a coderef, it is not passed to
# pg_rewind: it is run after pg_rewind finishes but before the target is
# restarted, e.g. to undo a test's own tampering with the target's data
# directory that pg_rewind itself doesn't need to be aware of.
sub rewind_node
{
	my ($target, $source, $label, %opts) = @_;
	my @extra_args;

	$source->stop;
	$target->stop;

	push @extra_args, '--restore-target-wal' if $opts{'-restorewal'};

	my $tpgdata = $target->data_dir;
	my $tmp = PostgreSQL::Test::Utils::tempdir;
	copy("$tpgdata/postgresql.conf", "$tmp/target-postgresql.conf.tmp");

	command_ok(
		[
			'pg_rewind',
			'--debug',
			'--source-pgdata' => $source->data_dir,
			'--target-pgdata' => $target->data_dir,
			'--no-sync',
			'--config-file' => "$tmp/target-postgresql.conf.tmp",
			@extra_args,
		],
		$label);

	move("$tmp/target-postgresql.conf.tmp", "$tpgdata/postgresql.conf");
	chmod($target->group_access() ? 0640 : 0600, "$tpgdata/postgresql.conf")
	  or BAIL_OUT("unable to set permissions for $tpgdata/postgresql.conf");

	$target->start unless $opts{'-norestart'};
}

# Rewrite a node's TLI history file in the old 3-field format (no UUID), so
# that pg_rewind sees a zero UUID for that side, as if the node had been
# promoted by a server that predates the UUID feature.
sub strip_tli_uuid
{
	my ($node, $tli) = @_;
	my $histfile = sprintf("%s/pg_wal/%08X.history", $node->data_dir, $tli);
	open(my $fh, '<', $histfile) or die "cannot open $histfile: $!";
	my @lines = <$fh>;
	close $fh;
	open($fh, '>', $histfile) or die "cannot write $histfile: $!";
	for my $line (@lines)
	{
		chomp $line;
		my @f = split(/\t/, $line, 4);
		if (@f == 4)
		{

			# Drop the UUID field (index 2); keep parentTLI, switchpoint, reason.
			print $fh join("\t", $f[0], $f[1], $f[3]) . "\n";
		}
		else
		{
			print $fh "$line\n";
		}
	}
	close $fh;
}

# Remove a node's TLI history file entirely, simulating a node that has no local
# record of its own current-TLI promotion, for example, because it started
# streaming a timeline it never itself switched to. This means that it never had
# a reason to write (or fetch) a local copy of that history file.  Returns the
# removed file's content, so it can be restored with restore_tli_history().
#
# Note this simulation is imperfect: unlike a node that never had that reason,
# our node genuinely did promote and so still has the earlier TLI's real WAL
# segments on disk. Without the history file linking them, the server itself can
# no longer make sense of its own pg_wal directory and will fail to start --
# restore_tli_history() must be called before restarting the node.
sub remove_tli_history
{
	my ($node, $tli) = @_;
	my $histfile = sprintf("%s/pg_wal/%08X.history", $node->data_dir, $tli);
	open(my $fh, '<', $histfile) or die "cannot open $histfile: $!";
	local $/;
	my $content = <$fh>;
	close $fh;
	unlink($histfile) or die "cannot remove $histfile: $!";
	return $content;
}

# Restore a TLI history file previously removed by remove_tli_history().
sub restore_tli_history
{
	my ($node, $tli, $content) = @_;
	my $histfile = sprintf("%s/pg_wal/%08X.history", $node->data_dir, $tli);
	open(my $fh, '>', $histfile) or die "cannot write $histfile: $!";
	print $fh $content;
	close $fh;
}

# Helper function to create an origin node with a test table and a row containing
# the given label.  The node is started and ready for use as a source for
# standbys.
sub setup_origin
{
	my ($label) = @_;
	my $node = PostgreSQL::Test::Cluster->new($label);
	$node->init(allows_streaming => 1);
	$node->append_conf('postgresql.conf', "wal_keep_size = 320MB\n");
	$node->start;
	$node->safe_psql('postgres', "CREATE TABLE tbl (val text)");
	$node->safe_psql('postgres', "INSERT INTO tbl VALUES ('$label')");
	$node->safe_psql('postgres', 'CHECKPOINT');
	return $node;
}

# Helper function to create multiple standby nodes from the same origin node.
# Each standby gets its own backup and data directory, so that they will
# generate independent UUIDs on promotion even though they share the same
# timeline history up to the point of promotion.
sub setup_standbys_from_origin
{
	my ($origin, @names) = @_;
	my @standbys;
	for my $name (@names)
	{
		my $standby = PostgreSQL::Test::Cluster->new($name);
		$origin->backup($standby->name);
		$standby->init_from_backup($origin, $standby->name,
			has_streaming => 1);
		$standby->append_conf('postgresql.conf', "wal_keep_size = 320MB\n");
		$standby->set_standby_mode();
		$standby->start;
		push @standbys, $standby;
	}
	return @standbys;
}

# Helper function to wait for multiple standby nodes to catch up to the origin.
sub sync_standbys_with_origin
{
	my ($origin, @standbys) = @_;
	$origin->wait_for_catchup($_) for @standbys;
}

# Helper function to insert a row with the given label into a node's test table.
sub write_record
{
	my ($node, $label) = @_;
	$node->safe_psql('postgres', "INSERT INTO tbl VALUES ('$label')");
	$node->safe_psql('postgres', 'CHECKPOINT');
}

# Test that pg_rewind detects and handles two standbys that independently
# promoted to the same timeline ID.  Before the UUID-based divergence check,
# pg_rewind's same-TLI shortcut would incorrectly skip the rewind in this
# case, leaving the target's diverged WAL intact.
#
#   origin (TLI 1)
#       |
#       +--- node_a (TLI 1) --promote--> TLI 2, UUID-A  (target)
#       |
#       +--- node_b (TLI 1) --promote--> TLI 2, UUID-B  (source)
#
# pg_rewind must detect the UUID mismatch and rewind node_a to match node_b.

my $node_origin = setup_origin('origin');

# Create node_a and node_b from separate backups of origin so that each
# has its own data directory and will generate an independent UUID on promotion.
my ($node_a, $node_b) =
  setup_standbys_from_origin($node_origin, 'node_a', 'node_b');

# Wait for both standbys to catch up to origin, then stop origin.  After
# this point the two standbys are isolated and will promote independently.
sync_standbys_with_origin($node_origin, $node_a, $node_b);
$node_origin->stop;

# Promote both standbys.  Each lands on TLI 2 but generates a distinct UUID,
# so the resulting clusters are diverged even though they share a timeline ID.
$node_a->promote;
$node_b->promote;

# Insert a divergent row on each so the rewind has visible work to do.
write_record($node_a, 'in A');
write_record($node_b, 'in B');

rewind_node($node_a, $node_b,
	'pg_rewind detects independent same-TLI promotions');

my $result =
  $node_a->safe_psql('postgres', "SELECT val FROM tbl ORDER BY val");
is($result, "in B\norigin",
	'rewound node has source data, not its own divergent data');

$node_a->teardown_node;
$node_b->teardown_node;
$node_origin->teardown_node;

# Test that pg_rewind correctly rewinds across a TLI mismatch buried in a shared
# prefix of the timeline history.  The target has gone through three timelines
# (TLI 1 -> TLI 2 -> TLI 3) while the source independently promoted from TLI 1
# to what is numerically TLI 2 but with a different UUID (TLI 2').  The deepest
# common ancestor is therefore TLI 1, and pg_rewind must rewind the target all
# the way back to the end of TLI 1.
#
#   origin (TLI 1) --+-- node_x --promote--> TLI 2 -- node_a --promote--> TLI 3
#                    |                                  (target: TLI 1->TLI 2->TLI 3)
#                    +-- node_b --promote--> TLI 2'
#                                            (source: TLI 1->TLI 2')
#
# findCommonAncestorTimeline walks forward: TLI 1 entries match (UUID=0 on
# both sides), then TLI 2 vs TLI 2' match on tli and begin but differ on
# UUID, signalling independent promotions.  The algorithm therefore backs up
# to TLI 1 as the common ancestor and sets the divergence point to the end
# of TLI 1.

my $node_origin2 = setup_origin('origin2');

# node_x and node_b2 both start from the same TLI 1 baseline.
my ($node_x, $node_b2) =
  setup_standbys_from_origin($node_origin2, 'node_x', 'node_b2');

# Both standbys must be caught up to the same LSN before origin stops, so
# that TLI 2 and TLI 2' both begin at the same WAL position.
sync_standbys_with_origin($node_origin2, $node_x, $node_b2);
$node_origin2->stop;

# Promote node_x to TLI 2 (UUID-X) and insert a row.  node_b2 is still on
# TLI 1 and has not yet seen any TLI 2 WAL.
$node_x->promote;
write_record($node_x, 'x');

# Build node_a2 as a standby of node_x, then promote it to TLI 3.
my ($node_a2) = setup_standbys_from_origin($node_x, 'node_a2');

sync_standbys_with_origin($node_x, $node_a2);
$node_x->stop;

$node_a2->promote;

# Now promote node_b2 independently from TLI 1 to TLI 2' (UUID-B, != UUID-X).
$node_b2->promote;
write_record($node_b2, 'b');

# Rewind node_a2 (TLI 1->TLI 2->TLI 3) from node_b2 (TLI 1->TLI 2') in
# local mode.  The rewind must reach back to the end of TLI 1.
#
# node_a2 was initialised from a streaming backup of node_x taken after
# node_x had already completed segment 4 of TLI 2; that segment therefore
# does not appear in node_a2's pg_wal.  pg_rewind's backward scan for the
# last checkpoint before the divergence point needs that segment, so we
# point restore_command at node_x's pg_wal and use --restore-target-wal.
#
# Note: no row is inserted on TLI 3.  This is intentional: the only
# post-divergence table modification in the target's WAL is the 'x' INSERT
# on TLI 2.  On unpatched code the WAL scan would start from the TLI 2
# shutdown checkpoint (just before TLI 3), miss that earlier insert, and
# leave 'x' in place instead of replacing it with 'b'.
my $node_x_waldir = $node_x->data_dir . "/pg_wal";
if ($PostgreSQL::Test::Utils::windows_os)
{
	$node_x_waldir =~ s{\\}{\\\\}g;
	$node_a2->append_conf('postgresql.conf',
		qq(\nrestore_command = 'copy "$node_x_waldir\\\\%f" "%p"'\n));
}
else
{
	$node_a2->append_conf('postgresql.conf',
		qq(\nrestore_command = 'cp "$node_x_waldir/%f" "%p"'\n));
}

rewind_node(
	$node_a2, $node_b2,
	'pg_rewind rewinds across mismatched TLI 2 / TLI 2-prime to TLI 1',
	-restorewal => 1);
my $result2 =
  $node_a2->safe_psql('postgres', "SELECT val FROM tbl ORDER BY val");
is($result2, "b\norigin2",
	'rewound node reflects source history, not target TLI 2/TLI 3 data');

$node_a2->teardown_node;
$node_b2->teardown_node;
$node_x->teardown_node;
$node_origin2->teardown_node;

# Test that pg_rewind correctly detects a mismatch when one cluster's TLI 2
# history entry carries a zero UUID (old-format history file) while the other
# carries a real UUID.  The two clusters must have promoted independently, so
# pg_rewind must rewind to TLI 1 rather than accepting the same-TLI shortcut.
#
# Run both orientations:
#   (a) target has zero UUID, source has real UUID
#   (b) target has real UUID, source has zero UUID
#
# In both cases the setup is:
#
#   origin (TLI 1) --+-- node_p --promote--> TLI 2, UUID-P  (target)
#                    |
#                    +-- node_q --promote--> TLI 2, UUID-Q  (source)
#
# One side then has its history file rewritten to the old 3-field format so
# that its UUID reads as zero.  pg_rewind must treat zero-vs-nonzero as
# incompatible (they cannot be the same promotion) and rewind to TLI 1.

for my $strip_target (1, 0)
{
	my $zero_side = $strip_target ? 'target' : 'source';
	my $real_side = $strip_target ? 'source' : 'target';
	my $sfx = $strip_target ? 'zt' : 'zs';
	my $label =
	  "pg_rewind rewinds when $zero_side has zero UUID and $real_side has real UUID";

	my $node_origin3 = setup_origin("origin3_$sfx");
	my ($node_p, $node_q) =
	  setup_standbys_from_origin($node_origin3, "node_p_$sfx", "node_q_$sfx");

	sync_standbys_with_origin($node_origin3, $node_p, $node_q);
	$node_origin3->stop;

	$node_p->promote;
	$node_q->promote;

	write_record($node_p, 'in P');
	write_record($node_q, 'in Q');

	# Strip UUID from the chosen side to simulate a pre-UUID server.
	strip_tli_uuid($strip_target ? $node_p : $node_q, 2);

	rewind_node($node_p, $node_q, $label);
	my $result3 =
	  $node_p->safe_psql('postgres', "SELECT val FROM tbl ORDER BY val");
	is( $result3,
		"in Q\norigin3_$sfx",
		'rewound node has source data, not its own divergent row');

	$node_p->teardown_node;
	$node_q->teardown_node;
	$node_origin3->teardown_node;
}

# Test that pg_rewind detects independent promotions to TLI 3 when both
# clusters share a common TLI 1 -> TLI 2 history (same UUID) but independently
# promoted from TLI 2 to TLI 3, producing different TLI 3 UUIDs.
#
#   origin (TLI 1) --- node_mid --promote--> TLI 2, UUID-M
#                                                  |
#                                                  +-- node_c --promote--> TLI 3, UUID-C  (target)
#                                                  |
#                                                  +-- node_d --promote--> TLI 3', UUID-D  (source)
#
# The same-TLI shortcut compares entry[Nentries-2].tluuid on each side; that
# is the UUID of the TLI 3 promotion, which differs.  The full rewind path
# then walks the history forward: TLI 1 matches (same tli/begin/UUID-M at
# entry[0]), TLI 2 also matches (same tli/begin; UUID-M is the same on both
# sides at entry[0]), but TLI 3 vs TLI 3' differ at entry[1] (UUID-C != UUID-D),
# so the divergence point is set to the end of TLI 2.

my $node_origin4 = setup_origin('origin4');
my ($node_mid) = setup_standbys_from_origin($node_origin4, 'node_mid');

sync_standbys_with_origin($node_origin4, $node_mid);
$node_origin4->stop;

# Promote node_mid to TLI 2 and insert a row that both TLI 3 nodes will share.
$node_mid->promote;
write_record($node_mid, 'mid');

# node_c and node_d both start as standbys of node_mid so they share the same
# TLI 2 promotion UUID (UUID-M).
my ($node_c, $node_d) =
  setup_standbys_from_origin($node_mid, 'node_c', 'node_d');
sync_standbys_with_origin($node_mid, $node_c, $node_d);
$node_mid->stop;

# Promote both independently; each generates a distinct TLI 3 UUID.
$node_c->promote;
$node_d->promote;

write_record($node_c, 'c');
write_record($node_d, 'd');

rewind_node($node_c, $node_d,
	'pg_rewind detects independent TLI 3 / TLI 3-prime promotions sharing TLI 2'
);
my $result4 =
  $node_c->safe_psql('postgres', "SELECT val FROM tbl ORDER BY val");
is($result4, "d\nmid\norigin4",
	'rewound node has source TLI 3-prime data, not its own TLI 3 data');

$node_c->teardown_node;
$node_d->teardown_node;
$node_mid->teardown_node;
$node_origin4->teardown_node;

# Test that pg_rewind does not fail when the target has no local copy of its own
# current TLI's history file, for example, because it started streaming that TLI
# from a primary that had already promoted, without ever itself performing a
# promotion or otherwise obtaining a local copy of the file.
#
# Without that file, pg_rewind cannot read the promotion UUID for the shared TLI
# on the target side, and so cannot tell whether the two clusters are the same
# promotion or two independent ones.
#
# We check that no rewind does actually take place, not just that the error
# message is correct. For that reason, we create two divergent nodes for the
# test, remove the history file, and then attempt a rewind. If the code works
# correctly, the node should not be rewound.
my $node_origin5 = setup_origin('origin5');
my ($node_g, $node_h) =
  setup_standbys_from_origin($node_origin5, 'node_g', 'node_h');

sync_standbys_with_origin($node_origin5, $node_g, $node_h);
$node_origin5->stop;

$node_g->promote;
$node_h->promote;

write_record($node_g, 'in G');
write_record($node_h, 'in H');

my $saved_history = remove_tli_history($node_g, 2);

rewind_node(
	$node_g, $node_h,
	'pg_rewind does not fail when target history file is missing',
	-norestart => 1);

# Restore the history file of node_g before it gets restarted. We are testing
# that pg_rewind can handle a missing history file, but unlike the real-world
# case this simulates, the pg_wal directory of node_g still has its own genuine
# TLI 1 segments on disk and the server cannot start back up without the file
# linking them to TLI 2.
restore_tli_history($node_g, 2, $saved_history);

$node_g->restart;

my $result5 =
  $node_g->safe_psql('postgres', "SELECT val FROM tbl ORDER BY val");
is( $result5,
	"in G\norigin5",
	'missing history file prevents divergence detection: target data left unchanged'
);

$node_g->teardown_node;
$node_h->teardown_node;
$node_origin5->teardown_node;

done_testing();
