# Copyright (c) 2026, PostgreSQL Global Development Group

# Test that a GUC_REPORT variable whose value cannot be converted to the
# client encoding is reported as a WARNING, does not fail the command or
# abort an open transaction, and does not leave the session stuck
# retrying the report.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
# The euro sign (U+20AC) exists in UTF8 but not in LATIN1.
$node->init(extra => [ '--encoding=UTF8', '--locale=C' ]);
$node->start;

my $euro_schema = "U&\"\\20AC\"";
my $conv_error = qr/has no equivalent in encoding "LATIN1"/;

# Use a persistent session so that a backend stuck retrying the report shows
# up as a query timeout.
my $psql = $node->background_psql('postgres', on_error_stop => 0);

$psql->query_safe("SET client_encoding = 'LATIN1'");

$psql->query("SET search_path = $euro_schema");
like(
	$psql->{stderr},
	qr/WARNING:  could not report parameter "search_path" to client/,
	'unreportable search_path produces a WARNING');
like($psql->{stderr}, $conv_error, 'WARNING detail names the conversion failure');
unlike($psql->{stderr}, qr/ERROR/, 'unreportable search_path is not an ERROR');

$psql->{stderr} = '';
my $alive = $psql->query_safe('SELECT 42');
like($alive, qr/\b42\b/, 'session is still responsive after the failed report');

# The SET did take effect on the server.
$psql->{stderr} = '';
$psql->query_safe("SET client_encoding = 'UTF8'");
my $sp = $psql->query_safe('SHOW search_path');
like($sp, qr/\x{e2}\x{82}\x{ac}|\x{20ac}/, 'search_path was set despite the failed report');
$psql->query_safe("SET client_encoding = 'LATIN1'");

# A later, representable change is still reported.
$psql->{stderr} = '';
$psql->query('SET search_path = public');
is($psql->{stderr}, '', 'a representable search_path change reports cleanly');

# An unreportable value set inside a transaction block must not abort it.
$psql->{stderr} = '';
$psql->query("BEGIN; SET search_path = $euro_schema; SELECT 'still open'");
like($psql->{stderr}, qr/WARNING/, 'in-transaction unreportable value warns');
unlike($psql->{stderr}, qr/ERROR|aborted/, 'in-transaction unreportable value does not abort');
$psql->{stderr} = '';
my $commit = $psql->query('COMMIT');
is($psql->{stderr}, '', 'transaction commits after the failed report');

$psql->quit;

$node->safe_psql('postgres', 'SELECT 1');
ok(1, 'server survived the unreportable search_path');

$node->stop;

done_testing();
