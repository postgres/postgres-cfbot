# Copyright (c) 2026, PostgreSQL Global Development Group

# FileOpsRename() renames at once, so the new name is usable inside the
# transaction, and the rename is reversed like every other immediate FILEOPS
# operation: on ROLLBACK, ROLLBACK TO SAVEPOINT and ROLLBACK PREPARED, by the
# startup undo phase after a crash, and on a standby that replays the abort.
# At wal_level = minimal an abort-time action renames it back.  A rename onto
# an existing name, or of a missing file, is refused and changes nothing.
# Crash redo of committed renames converges on the state before the crash.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $injection = $ENV{enable_injection_points} eq 'yes';

my $node = PostgreSQL::Test::Cluster->new('fileops_rename');
$node->init(allows_streaming => 1);
$node->append_conf(
	'postgresql.conf', q{
autovacuum = off
max_prepared_transactions = 5
});
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'injection_points'\n")
  if $injection;
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
my $datadir = $node->data_dir;

# Commit file $name holding $content on $n.  FILEOPS paths are relative to
# the data directory, and these are WAL-logged, so a standby gets them.
sub mkfile
{
	my ($n, $name, $content) = @_;
	$n->safe_psql(
		'postgres', qq{
BEGIN;
SELECT test_fileops_create('$name', 384);
SELECT test_fileops_write('$name', 0, '$content');
COMMIT;
});
}

# What file $name in $dir holds, or '<missing>'.
sub content
{
	my ($dir, $name) = @_;
	return -f "$dir/$name" ? slurp_file("$dir/$name") : '<missing>';
}

# Rename $from to $to in $bg's open transaction; it must show at once.
sub rename_in
{
	my ($bg, $dir, $from, $to, $tag) = @_;
	$bg->query_safe("SELECT test_fileops_rename('$from', '$to')");
	ok( !-e "$dir/$from" && -e "$dir/$to",
		"$tag: renamed inside the transaction");
}

my $bg = $node->background_psql('postgres', on_error_stop => 0);

# a. COMMIT keeps the rename
mkfile($node, 'a.dat', 'content-a');
$bg->query_safe('BEGIN');
rename_in($bg, $datadir, 'a.dat', 'a2.dat', 'a');
is(content($datadir, 'a2.dat'),
	'content-a', 'a: the new name has the content inside the transaction');
$bg->query_safe('COMMIT');
is(content($datadir, 'a2.dat'), 'content-a', 'a: COMMIT keeps the new name');
ok(!-e "$datadir/a.dat", 'a: COMMIT keeps the old name gone');

# b. ROLLBACK restores the old name
mkfile($node, 'b.dat', 'content-b');
$bg->query_safe('BEGIN');
rename_in($bg, $datadir, 'b.dat', 'b2.dat', 'b');
$bg->query_safe('ROLLBACK');
is(content($datadir, 'b.dat'),
	'content-b', 'b: ROLLBACK restores the old name with its content');
ok(!-e "$datadir/b2.dat", 'b: ROLLBACK removes the new name');

# c. ROLLBACK TO SAVEPOINT restores it, and COMMIT does not redo it
mkfile($node, 'c.dat', 'content-c');
$bg->query_safe('BEGIN');
$bg->query_safe('SAVEPOINT s');
rename_in($bg, $datadir, 'c.dat', 'c2.dat', 'c');
$bg->query_safe('ROLLBACK TO SAVEPOINT s');
is(content($datadir, 'c.dat'),
	'content-c', 'c: ROLLBACK TO SAVEPOINT restores the old name');
ok(!-e "$datadir/c2.dat", 'c: ROLLBACK TO SAVEPOINT removes the new name');
$bg->query_safe('COMMIT');
is(content($datadir, 'c.dat'), 'content-c', 'c: still restored after COMMIT');
ok(!-e "$datadir/c2.dat", 'c: new name still absent after COMMIT');

# d. a later operation on the new name is undone first (LIFO)
mkfile($node, 'd.dat', 'content-d');
my ($ret, $stdout, $stderr) = $node->psql(
	'postgres', q{BEGIN;
SELECT test_fileops_rename('d.dat', 'd2.dat');
SELECT test_fileops_write('d2.dat', 0, 'CHANGED-AND-LONGER');
SELECT pg_read_file('d2.dat');
ROLLBACK;});
like($stdout, qr/^CHANGED-AND-LONGER$/m, 'd: the write went to the new name');
is(content($datadir, 'd.dat'),
	'content-d', 'd: ROLLBACK undid the write, then the rename');
ok(!-e "$datadir/d2.dat", 'd: new name absent after ROLLBACK');

# e. refused: an existing destination, a missing source
mkfile($node, 'e.dat', 'content-e');
mkfile($node, 'e2.dat', 'content-e2');
($ret, $stdout, $stderr) =
  $node->psql('postgres', "SELECT test_fileops_rename('e.dat', 'e2.dat')");
isnt($ret, 0, 'e: renaming onto an existing name fails');
like($stderr, qr/already exists/, 'e: ... saying the name exists');
is(content($datadir, 'e.dat'), 'content-e', 'e: source unchanged');
is(content($datadir, 'e2.dat'), 'content-e2', 'e: destination unchanged');
($ret, $stdout, $stderr) = $node->psql('postgres',
	"SELECT test_fileops_rename('e-missing.dat', 'e3.dat')");
isnt($ret, 0, 'e: renaming a missing file fails');
like($stderr, qr/could not stat file/, 'e: ... saying it is missing');
ok(!-e "$datadir/e3.dat", 'e: ... and creates nothing');

# f. a directory, with a file in it
$node->safe_psql('postgres', "SELECT test_fileops_mkdir('fdir', 448)");
mkfile($node, 'fdir/inner.dat', 'content-f');
$bg->query_safe('BEGIN');
rename_in($bg, $datadir, 'fdir', 'fdir2', 'f');
is(content($datadir, 'fdir2/inner.dat'),
	'content-f', 'f: the directory contents moved with it');
$bg->query_safe('ROLLBACK');
is(content($datadir, 'fdir/inner.dat'),
	'content-f', 'f: ROLLBACK restores the directory and its contents');
ok(!-e "$datadir/fdir2", 'f: new directory name absent after ROLLBACK');

# l. the rollback leaves an old name that was reused in the meantime alone
mkfile($node, 'l.dat', 'content-l');
$bg->query_safe('BEGIN');
rename_in($bg, $datadir, 'l.dat', 'l2.dat', 'l: old name reused');
append_to_file("$datadir/l.dat", 'reused');
$bg->query_safe('ROLLBACK');
is(content($datadir, 'l.dat'), 'reused', 'l: the reused old name is kept');
is(content($datadir, 'l2.dat'),
	'content-l', 'l: the renamed file stays under the new name');
$bg->quit;

# g. crash recovery reverses the rename of an in-flight transaction
mkfile($node, 'g.dat', 'content-g');
my $bgc = $node->background_psql('postgres', on_error_stop => 0);
$bgc->query_safe('BEGIN');
rename_in($bgc, $datadir, 'g.dat', 'g2.dat', 'g');
$node->stop('immediate');
$bgc->{run}->finish;
$node->start;
is(content($datadir, 'g.dat'),
	'content-g', 'g: crash recovery restores the old name');
ok(!-e "$datadir/g2.dat", 'g: crash recovery removes the new name');

# g. a committed rename of a file created since the last checkpoint: redo
# re-creates the old name from the earlier records, and the rename's redo must
# not leave it behind
mkfile($node, 'g3.dat', 'content-g3');
$node->safe_psql('postgres',
	"SELECT test_fileops_rename('g3.dat', 'g4.dat')");
$node->stop('immediate');
$node->start;
is(content($datadir, 'g4.dat'),
	'content-g3', 'g: a committed rename survives a crash');
ok(!-e "$datadir/g3.dat", 'g: ... and redo leaves no old name behind');

# g. committed history a -> b, b -> c, then b made again, where a predates the
# checkpoint.  Redo of b -> c finds the later b and c both present; renaming
# over c would replace the renamed file with the later b.
mkfile($node, 'ga.dat', 'content-ga');
$node->safe_psql('postgres', 'CHECKPOINT');
$node->safe_psql('postgres',
	"SELECT test_fileops_rename('ga.dat', 'gb.dat')");
$node->safe_psql('postgres',
	"SELECT test_fileops_rename('gb.dat', 'gc.dat')");
mkfile($node, 'gb.dat', 'new-gb');
$node->stop('immediate');
$node->start;
ok(!-e "$datadir/ga.dat", 'g: a -> b -> c, b again: no a after the crash');
is(content($datadir, 'gb.dat'), 'new-gb', 'g: ... b is the later file');
is(content($datadir, 'gc.dat'),
	'content-ga', 'g: ... c is still the renamed file');

# g. a renamed directory that redo re-creates and fills under its old name is
# not removed: both names are left, with a WARNING, and nothing is lost
$node->safe_psql('postgres', "SELECT test_fileops_mkdir('gdir', 448)");
mkfile($node, 'gdir/x.dat', 'content-gx');
$node->safe_psql('postgres', "SELECT test_fileops_rename('gdir', 'gdir2')");
my $offset = -s $node->logfile;
$node->stop('immediate');
$node->start;
is(content($datadir, 'gdir2/x.dat'),
	'content-gx', 'g: a renamed directory keeps its contents after a crash');
like(
	slurp_file($node->logfile, $offset),
	qr/WARNING: .*"gdir"/,
	'g: redo warns about the re-created old directory it leaves');

# h. two-phase commit
mkfile($node, 'h1.dat', 'content-h1');
$node->safe_psql('postgres',
	"BEGIN; SELECT test_fileops_rename('h1.dat', 'h1b.dat'); PREPARE TRANSACTION 'h1';"
);
ok(!-e "$datadir/h1.dat" && -e "$datadir/h1b.dat",
	'h: renamed while prepared');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'h1'");
is(content($datadir, 'h1.dat'),
	'content-h1', 'h: ROLLBACK PREPARED restores the old name');
ok(!-e "$datadir/h1b.dat", 'h: ROLLBACK PREPARED removes the new name');

mkfile($node, 'h2.dat', 'content-h2');
$node->safe_psql('postgres',
	"BEGIN; SELECT test_fileops_rename('h2.dat', 'h2b.dat'); PREPARE TRANSACTION 'h2';"
);
$node->stop('immediate');
$node->start;
ok( !-e "$datadir/h2.dat" && -e "$datadir/h2b.dat",
	'h: still renamed while prepared after a crash');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'h2'");
is(content($datadir, 'h2.dat'),
	'content-h2', 'h: ROLLBACK PREPARED after a crash restores the old name');
ok(!-e "$datadir/h2b.dat",
	'h: ROLLBACK PREPARED after a crash removes the new name');

mkfile($node, 'h3.dat', 'content-h3');
$node->safe_psql('postgres',
	"BEGIN; SELECT test_fileops_rename('h3.dat', 'h3b.dat'); PREPARE TRANSACTION 'h3';"
);
$node->safe_psql('postgres', "COMMIT PREPARED 'h3'");
is(content($datadir, 'h3b.dat'),
	'content-h3', 'h: COMMIT PREPARED keeps the new name');
ok(!-e "$datadir/h3.dat", 'h: COMMIT PREPARED keeps the old name gone');

# i. a streaming standby converges, for COMMIT and for ROLLBACK
$node->backup('bk');
my $standby = PostgreSQL::Test::Cluster->new('fileops_rename_standby');
$standby->init_from_backup($node, 'bk', has_streaming => 1);
$standby->start;
my $sdir = $standby->data_dir;
mkfile($node, 'i1.dat', 'content-i1');
mkfile($node, 'i2.dat', 'content-i2');
$node->safe_psql('postgres',
	"SELECT test_fileops_rename('i1.dat', 'i1b.dat')");
my $bgi = $node->background_psql('postgres', on_error_stop => 0);
$bgi->query_safe('BEGIN');
$bgi->query_safe("SELECT test_fileops_rename('i2.dat', 'i2b.dat')");
$node->wait_for_catchup($standby);
is(content($sdir, 'i1b.dat'),
	'content-i1', 'i: standby has the committed new name');
ok(!-e "$sdir/i1.dat", 'i: standby lost the committed old name');
ok( !-e "$sdir/i2.dat" && -e "$sdir/i2b.dat",
	'i: standby shows a rename in flight');
$bgi->query_safe('ROLLBACK');
$bgi->quit;
$node->wait_for_catchup($standby);
is(content($sdir, 'i2.dat'),
	'content-i2', 'i: standby restores the old name of a rolled-back rename');
ok(!-e "$sdir/i2b.dat", 'i: standby has no new name of a rolled-back rename');

# i. ROLLBACK PREPARED on a promoted standby renames back, though the
# standby's file is not the primary's inode
mkfile($node, 'i3.dat', 'content-i3');
$node->safe_psql('postgres',
	"BEGIN; SELECT test_fileops_rename('i3.dat', 'i3b.dat'); PREPARE TRANSACTION 'i3';"
);
$node->wait_for_catchup($standby);
ok( !-e "$sdir/i3.dat" && -e "$sdir/i3b.dat",
	'i: standby shows the prepared rename');
$standby->promote;
$standby->safe_psql('postgres', "ROLLBACK PREPARED 'i3'");
is(content($sdir, 'i3.dat'),
	'content-i3',
	'i: ROLLBACK PREPARED on the promoted standby restores the old name');
ok(!-e "$sdir/i3b.dat",
	'i: ROLLBACK PREPARED on the promoted standby removes the new name');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'i3'");
$standby->stop;

# j. wal_level = minimal: no WAL, an abort-time action renames back
my $min = PostgreSQL::Test::Cluster->new('fileops_rename_minimal');
$min->init;
$min->append_conf('postgresql.conf',
	"wal_level = minimal\nmax_wal_senders = 0\n");
$min->start;
$min->safe_psql('postgres', 'CREATE EXTENSION test_fileops');
is($min->safe_psql('postgres', 'SHOW wal_level'),
	'minimal', 'j: wal_level is minimal');
my $mdir = $min->data_dir;
mkfile($min, 'j.dat', 'content-j');
my $bgm = $min->background_psql('postgres', on_error_stop => 0);
$bgm->query_safe('BEGIN');
rename_in($bgm, $mdir, 'j.dat', 'j2.dat', 'j');
$bgm->query_safe('ROLLBACK');
is(content($mdir, 'j.dat'), 'content-j', 'j: ROLLBACK restores the old name');
ok(!-e "$mdir/j2.dat", 'j: ROLLBACK removes the new name');
$bgm->query_safe('BEGIN');
$bgm->query_safe('SAVEPOINT s');
rename_in($bgm, $mdir, 'j.dat', 'j2.dat', 'j: subtransaction');
$bgm->query_safe('ROLLBACK TO SAVEPOINT s');
is(content($mdir, 'j.dat'),
	'content-j', 'j: ROLLBACK TO SAVEPOINT restores the old name');
rename_in($bgm, $mdir, 'j.dat', 'j3.dat', 'j: then');
$bgm->query_safe('COMMIT');
is(content($mdir, 'j3.dat'), 'content-j', 'j: COMMIT keeps the new name');
ok(!-e "$mdir/j.dat" && !-e "$mdir/j2.dat", 'j: and no other name');
$bgm->quit;
$min->stop;

# k. a crash after the WAL is flushed but before rename(): redo performs the
# rename, and the loser's UNDO reverses it
SKIP:
{
	skip 'Injection points not supported by this build', 4 unless $injection;
	$node->safe_psql('postgres', 'CREATE EXTENSION injection_points');
	mkfile($node, 'k.dat', 'content-k');
	$node->safe_psql('postgres',
		"SELECT injection_points_attach('fileops-rename-after-wal-flush', 'wait')"
	);
	my $bgk = $node->background_psql('postgres', on_error_stop => 0);
	$bgk->query_until(
		qr/start/, q(\echo start
BEGIN;
SELECT test_fileops_rename('k.dat', 'k2.dat');
));
	$node->wait_for_event('client backend', 'fileops-rename-after-wal-flush');
	ok(-e "$datadir/k.dat" && !-e "$datadir/k2.dat",
		'k: logged, not yet renamed');
	$offset = -s $node->logfile;
	$node->stop('immediate');
	$bgk->{run}->finish;
	$node->start;
	like(
		slurp_file($node->logfile, $offset),
		qr/UNDO recovery: rolling back transaction/,
		'k: the transaction was rolled back as a loser');
	is(content($datadir, 'k.dat'), 'content-k', 'k: the old name is kept');
	ok(!-e "$datadir/k2.dat", 'k: the new name is absent');
}

$node->stop;

done_testing();
