

# Copyright (c) 2026, PostgreSQL Global Development Group
#
# FLUX DELETE after an aborted out-of-place UPDATE, under
# wal_consistency_checking.
#
# An out-of-place UPDATE leaves the old version DELETED | SUPERSEDED with
# t_ctid naming the new version.  If it aborts and its rollback is deferred,
# that image stays on the page.  A later DELETE must not act on it: it reverts
# the aborted version first (flux_help_revert), so it deletes the restored
# before-image, which carries neither SUPERSEDED nor the forward link.  The
# DELETE and its redo leave SUPERSEDED clear and t_ctid at the tuple's own
# TID; flux_mask compares SUPERSEDED, and t_ctid with it.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init(allows_streaming => 1);

# No undo apply worker can start (max_worker_processes is the number of
# static workers, see 076_flux_deferred_rollback.pl), and every rollback is
# deferred to it, so the aborted UPDATE stays on the page.
$node->append_conf(
	'postgresql.conf', qq(
autovacuum = off
wal_consistency_checking = all
max_logical_replication_workers = 0
max_logical_revert_workers = 0
max_parallel_workers = 0
max_parallel_workers_per_gather = 0
max_worker_processes = 3
undo_rollback_overflow_size = 0
));
$node->start;
$node->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($node, 'bkp', has_streaming => 1);
$standby->start;

sub dump_tup
{
	my ($n, $rel, $label, $tid) = @_;
	my ($blk, $off) = $tid =~ /\((\d+),(\d+)\)/;
	my $hex = $n->safe_psql('postgres',
		"SELECT encode(get_raw_page('$rel', $blk), 'hex')");
	my $raw = pack('H*', $hex);
	my $lp = unpack('V', substr($raw, 24 + 4 * ($off - 1), 4));
	my $lpoff = $lp & 0x7fff;
	my $flags = unpack('v', substr($raw, $lpoff + 32, 2));
	my ($bhi, $blo, $o) = unpack('vvv', substr($raw, $lpoff + 24, 6));
	my $xmax = unpack('V', substr($raw, $lpoff, 4));
	my $ctid = sprintf("(%u,%u)", ($bhi << 16) | $blo, $o);
	note sprintf("%s %s: t_flags=0x%04x t_ctid=%s xmax=%u",
		$label, $tid, $flags, $ctid, $xmax);
	is($ctid, $tid, "$label: t_ctid is the own TID") if !($flags & 0x1000);
	return $flags;
}

# t: a deferrable unique constraint cannot be maintained in place, so a
# change of k takes the out-of-place (new-TID) path.  u: the same, on a row a
# committed in-place key UPDATE left KEYS_UPDATED (the constraint is added
# afterwards), for which a keyless uniqueness fetch does not take the
# committed-version path.  In both, a uniqueness check meets the aborted
# version before the DELETE.
$node->safe_psql(
	'postgres', q{
CREATE EXTENSION pageinspect;
CREATE TABLE t (id int PRIMARY KEY, k int UNIQUE DEFERRABLE, v int) USING flux;
INSERT INTO t SELECT g, g, g * 10 FROM generate_series(1, 10) g;
CREATE TABLE u (id int PRIMARY KEY, k int, v int) USING flux;
CREATE INDEX u_k ON u (k);
INSERT INTO u SELECT g, g, g * 10 FROM generate_series(1, 10) g;
UPDATE u SET k = 1001 WHERE id = 1;
ALTER TABLE u ADD UNIQUE (k) DEFERRABLE;
CHECKPOINT;
});

foreach my $tab (qw(t u))
{
	my $old_tid =
	  $node->safe_psql('postgres', "SELECT ctid FROM $tab WHERE id = 1");

	$node->safe_psql('postgres',
		"BEGIN; UPDATE $tab SET k = 100 WHERE id = 1; ROLLBACK;");
	ok( dump_tup($node, $tab, "$tab after abort", $old_tid) & 0x1000,
		"$tab: the aborted out-of-place UPDATE left SUPERSEDED on the page");

	# The PK uniqueness check meets the old version: the row is live.
	my ($ret, $out, $err) =
	  $node->psql('postgres', "INSERT INTO $tab VALUES (1, 1000, 0)");
	like($err, qr/duplicate key value/,
		"$tab: row 1 is live after the abort");
	dump_tup($node, $tab, "$tab after unique check", $old_tid);

	is( $node->safe_psql(
			'postgres', "DELETE FROM $tab WHERE id = 1 RETURNING ctid"),
		$old_tid,
		"$tab: DELETE removes the old version");
	ok(!(dump_tup($node, $tab, "$tab after delete", $old_tid) & 0x1000),
		"$tab: primary: the DELETE cleared SUPERSEDED");

	$node->wait_for_replay_catchup($standby);
	is( $standby->safe_psql(
			'postgres', "SELECT count(*) FROM $tab WHERE id = 1"),
		'0',
		"$tab: standby: row 1 is deleted");
	ok( !(  dump_tup($standby, $tab, "$tab standby after delete", $old_tid) &
			0x1000),
		"$tab: standby: the DELETE redo cleared SUPERSEDED");
}
is( $node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_stat_activity
		  WHERE backend_type = 'undo apply worker'}),
	'0',
	'no undo apply worker');
unlike(
	slurp_file($standby->logfile),
	qr/inconsistent page found/,
	'standby: FLUX DELETE redo reproduces the primary page');

# A non-key move records compatibility on the old version.  Exercise both
# the first change after a checkpoint and a later non-FPI SUPERSEDE record.
$node->safe_psql(
	'postgres', q{
CREATE TABLE nk (id int, v int, PRIMARY KEY (id) INCLUDE (v)) USING flux;
INSERT INTO nk VALUES (1, 1), (2, 1);
CHECKPOINT;
});
foreach my $id (1, 2)
{
	my $tid =
	  $node->safe_psql('postgres', "SELECT ctid FROM nk WHERE id=$id");
	$node->safe_psql('postgres', "UPDATE nk SET v=2 WHERE id=$id");
	ok(dump_tup($node, 'nk', "non-key move $id primary", $tid) & 0x2000,
		"non-key move $id records key compatibility");
	$node->wait_for_replay_catchup($standby);
	ok(dump_tup($standby, 'nk', "non-key move $id standby", $tid) & 0x2000,
		"non-key move $id replays key compatibility");
}
is($standby->safe_psql('postgres', 'SELECT sum(v) FROM nk'),
	'4', 'standby sees both moved rows');
unlike(
	slurp_file($standby->logfile),
	qr/inconsistent page found/,
	'non-key move redo reproduces the primary page');

$standby->stop;
$node->stop;
done_testing();
