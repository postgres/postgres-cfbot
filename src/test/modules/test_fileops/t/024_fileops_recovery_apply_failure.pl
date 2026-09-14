# Copyright (c) 2026, PostgreSQL Global Development Group

# The startup undo phase must not report a crash loser as rolled back when one
# of its UNDO records could not be applied.
#
# A crash loser changed a file's mode through FILEOPS.  Before the restart the
# file's directory loses its search permission, so the inverse chmod() fails
# with a real EACCES.  Nothing retries it, so the mode stays changed; the
# summary at the end of the undo phase has to say so.
#
# Must not run as root (root ignores the directory permission).
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'running as root: EACCES cannot be produced' if $> == 0;
plan skip_all => 'directory search permission is POSIX-only'
  if $PostgreSQL::Test::Utils::windows_os;

my $node = PostgreSQL::Test::Cluster->new('fileops_recovery_apply_failure');
$node->init;
# FILEOPS writes UNDO only when WAL is needed.
$node->append_conf('postgresql.conf', "wal_level = replica\nautovacuum = off\n");
$node->start;
$node->safe_psql('postgres', 'CREATE EXTENSION test_fileops');

my $dir = PostgreSQL::Test::Utils::tempdir('eacces');
my $path = "$dir/m.dat";
open(my $fh, '>', $path) or die "open $path: $!";
close($fh);
chmod(0644, $path) or die "chmod $path: $!";

# A chmod in flight when the server crashes: a crash loser.
my $bg = $node->background_psql('postgres', on_error_stop => 0);
$bg->query_safe('BEGIN');
$bg->query_safe("SELECT test_fileops_chmod('$path', 384)");    # 0600
$node->safe_psql('postgres', 'SELECT pg_switch_wal()');
my $offset = -s $node->logfile;
$node->stop('immediate');
$bg->{run}->finish;

chmod(0600, $dir) or die "chmod $dir: $!";
$node->start;
chmod(0700, $dir) or die "chmod $dir: $!";

my $log = slurp_file($node->logfile, $offset);
like($log, qr/FILEOPS UNDO CHMOD: could not restore mode of .*: Permission denied/,
	'the inverse failed with a real EACCES');
like($log, qr/UNDO recovery complete: 1 transactions processed, 0 records applied, 1 records could not be applied/,
	'the undo phase reports the record it could not apply');
unlike($log, qr/UNDO recovery complete: \d+ transactions rolled back/,
	'the loser is not reported as rolled back');
is(sprintf('%04o', (stat($path))[2] & 07777), '0600',
	'the mode change was not rolled back');

$node->stop;
done_testing();
