# Copyright (c) 2026, PostgreSQL Global Development Group
# These are source-contract checks, not an fsync fault-injection test.
use strict;
use warnings FATAL => 'all';
use FindBin;
use PostgreSQL::Test::Utils;
use Test::More;

my $src = "$FindBin::RealBin/../../../..";
# 016_fileops_inverse_fsync.pl tests what a failed inverse sync does, and
# 017_fileops_inverse_fsync_batch.pl that each path and its directory are
# synced once per rollback.
my $header = slurp_file("$src/include/access/xlog_internal.h");
unlike(
	$header,
	qr/#define XLOG_PAGE_MAGIC 0xD125\b/,
	'incompatible FILEOPS and ATM records change WAL magic');
done_testing();
