
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(usleep);

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $psql = $node->background_psql('postgres');

# Confirm connection works and that disconnect has not been requested yet.
my $result = $psql->query_safe("SELECT 'before_shutdown'");
like($result, qr/before_shutdown/, 'connection works before smart shutdown');
is($psql->query_safe("SHOW disconnect_requested"),
	'off', 'disconnect_requested is off before smart shutdown');

# Initiate smart shutdown without waiting for it to complete
$node->command_ok(
	[ 'pg_ctl', 'stop', '-D', $node->data_dir, '-m', 'smart', '--no-wait' ],
	'pg_ctl smart shutdown');

# Once the backend processes the smart shutdown signal it turns on the
# disconnect_requested GUC, which is reported to the client through a
# ParameterStatus message.  psql logs a notice the first time it observes this.
# Poll with queries until psql reports it.
my $saw_request = 0;
for (my $i = 0; $i < 100; $i++)
{
	my $out = $psql->query("SELECT 'after_shutdown'");
	if ($psql->{stderr} =~
		/Server requested graceful disconnect when convenient/)
	{
		$saw_request = 1;
		# The query should still have succeeded
		like($out, qr/after_shutdown/,
			'query still works after disconnect was requested');
		last;
	}
	usleep(50_000);
}
ok($saw_request,
	'psql reported graceful disconnect request during smart shutdown');

# The GUC is now reported as on for this session too.  Use query() rather than
# query_safe(), since psql may emit the advisory notice again on stderr.
like($psql->query("SHOW disconnect_requested"),
	qr/^on$/m, 'disconnect_requested is on after smart shutdown');

$psql->quit;

done_testing();
