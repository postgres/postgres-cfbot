# Copyright (c) 2026, PostgreSQL Global Development Group
# These are source-contract checks, not an fsync fault-injection test.
use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Utils;
use Test::More;

my $src = "$FindBin::RealBin/../../../..";
my $undo = slurp_file("$src/backend/storage/file/fileops_undo.c");
$undo =~ /void\s+FileOpsApplyInverse\(.*?(?=fileops_undo_desc -)/s
  or die 'cannot find inverse applier';
my $apply = $&;
my @syncs = $apply =~ /(fsync_fname_ext\([^;]+;)/g;
is(scalar @syncs, 2, 'inverse syncs file and parent');
# 016_fileops_inverse_fsync.pl tests what a failed inverse sync does.
my $header = slurp_file("$src/include/access/xlog_internal.h");
unlike($header, qr/#define XLOG_PAGE_MAGIC 0xD125\b/,
 'incompatible FILEOPS and ATM records change WAL magic');
done_testing();
