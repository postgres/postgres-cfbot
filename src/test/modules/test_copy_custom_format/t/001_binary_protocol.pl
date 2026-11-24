
# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a custom COPY format declared as binary is sent with the binary
# format code in the COPY protocol, so that psql passes the data through
# as-is rather than reading it line by line.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_copy_custom_format'");
$node->start;

$node->safe_psql('postgres', 'CREATE TABLE copy_data (a int)');

# Input data containing a NUL byte and a line that looks like the end-of-data
# marker.  Both must reach the server unchanged.
my $data = "abc\0def\n\\.\nghi\n";
my $len = length($data);

my ($stdout, $stderr);
$node->psql(
	'postgres',
	"COPY copy_data FROM STDIN WITH (format 'test_binary_format');\n" . $data,
	stdout => \$stdout,
	stderr => \$stderr);
like(
	$stderr,
	qr/CopyFromOneRow: read $len bytes/,
	'binary custom format receives all the input data');

$node->stop;

done_testing();
