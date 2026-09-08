
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

# Regression test for a COPY FROM (FORMAT text) hang introduced by the SIMD
# input scanner (commit e0a3a3fd536).
#
# CopyReadLineTextSIMDHelper() refills the input buffer once fewer than
# sizeof(Vector8) bytes remain, before falling back to the scalar parser.  If
# those buffered bytes already hold a complete "\." end-of-copy marker and the
# source is a still-open pipe with no further data ready, the refill blocks and
# COPY fails to recognize the marker until the writer produces more data or
# closes the pipe.  The fix checks the short tail for a backslash and defers to
# the scalar parser, which recognizes the marker without reading ahead.
#
# The writer program below emits exactly one full input buffer
# (INPUT_BUF_SIZE = 65536 bytes): a long line, a complete "\." marker, and a
# few trailing bytes so the marker lands in the short (< sizeof(Vector8)) tail.
# It then keeps the pipe open with a slow dribble so a buggy refill blocks
# (rather than seeing EOF) until statement_timeout would fire.  With the fix,
# COPY recognizes the marker immediately and the writer exits on SIGPIPE.

use strict;
use warnings FATAL => 'all';
use File::Spec;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($windows_os)
{
	plan skip_all =>
	  'COPY FROM PROGRAM pipe-blocking scenario is unreliable on Windows';
}

# The writer script, run by COPY ... FROM PROGRAM.  Single-quoted heredoc: the
# contents are written to the file verbatim, so the "\." below reaches the
# child perl as a literal backslash-dot end-of-copy marker.
my $writer = <<'END_WRITER';
$SIG{PIPE} = sub { exit 0 };
my $data = ("a" x 65524) . "\n\\.\n" . ("x" x 8);
die "payload is not 65536 bytes" unless length($data) == 65536;
my $offset = 0;
while ($offset < length($data))
{
	my $written = syswrite(STDOUT, $data, length($data) - $offset, $offset);
	die "write failed: $!" unless defined $written;
	$offset += $written;
}
while (1)
{
	select(undef, undef, undef, 0.05);
	syswrite(STDOUT, "x");
}
END_WRITER

# Absolute path: COPY ... FROM PROGRAM runs in the backend's data directory,
# not the test's working directory.
my $script = File::Spec->rel2abs(
	"${PostgreSQL::Test::Utils::tmp_check}/copy_simd_eoc_writer.pl");
append_to_file($script, $writer);

my $node = PostgreSQL::Test::Cluster->new('primary');
$node->init;
$node->start;

# statement_timeout bounds the failure: a buggy build blocks in the refill and
# aborts here; a fixed build recognizes the marker and returns immediately,
# independent of the timeout.
my $sql = qq{
SET statement_timeout = '30s';
CREATE TEMP TABLE copy_simd_eoc (a text);
COPY copy_simd_eoc FROM PROGRAM '$^X "$script"';
SELECT count(*) || '|' || min(length(a)) FROM copy_simd_eoc;
};

my ($stdout, $stderr) = ('', '');
my $ret = $node->psql(
	'postgres', $sql,
	stdout => \$stdout,
	stderr => \$stderr,
	timeout => $PostgreSQL::Test::Utils::timeout_default);

is($ret, 0,
	'COPY FROM pipe with a buffered end-of-copy marker does not block');
is($stdout, '1|65524', 'exactly one row of the expected length was copied');
diag("psql stderr: $stderr") if $ret != 0;

$node->stop;

done_testing();
