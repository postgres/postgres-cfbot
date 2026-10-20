# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

if ($windows_os)
{
	plan skip_all => 'sending SIGINT on Windows terminates the test itself';
}

eval { require IO::Pty; };
if ($@)
{
	plan skip_all => 'IO::Pty is needed to run this test';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $script = $node->basedir . '/prompt.sql';
append_to_file($script, "SELECT 111; \\prompt 'ANSWER: ' answer\n");

my $confirm = qr/return to cancel[^\r\n]*\r?\n/;
my $ready = qr/postgres=# /;

for my $no_readline (1, 0)
{
	# Respect the same readline opt-out as 010_tab_completion.pl.
	next
	  if !$no_readline
	  && ((!defined($ENV{with_readline}) || $ENV{with_readline} ne 'yes')
		|| defined($ENV{SKIP_READLINE_TESTS}));

	my $mode = $no_readline ? 'without readline' : 'with readline';
	my $h = $node->interactive_psql('postgres',
		extra_params => $no_readline ? ['-n'] : []);
	$h->set_query_timer_restart;
	$h->query_until(qr/step-ready\r?\npostgres=# /,
		"\\set SINGLESTEP on\n\\echo step-ready\n");

	# After singlestep_verify returns, Ctrl-C during \prompt on the same
	# line must use the context saved by sigsetjmp in MainLoop.
	for my $response ("\n", "x\n")
	{
		my $action = $response eq "\n" ? 'accepting' : 'skipping';
		$h->query_until($confirm, "\\i $script\n");
		$h->query_until(qr/ANSWER: /, $response);
		$h->{run}->signal('INT');
		$h->query_until($ready, '');

		$h->query_until($confirm, "SELECT 222;\n");
		like($h->query_until($ready, "\n"),
			qr/\r?\n222\r?\n/,
			"session survives canceling \\prompt after $action, $mode");
	}

	# Canceling confirmation must leave the session usable for another query.
	$h->query_until($confirm, "SELECT 333;\n");
	$h->{run}->signal('INT');
	$h->query_until($ready, '');
	$h->query_until($confirm, "SELECT 444;\n");
	like($h->query_until($ready, "\n"),
		qr/\r?\n444\r?\n/,
		"session survives canceling single-step confirmation, $mode");

	ok($h->quit, "psql exits normally, $mode");
}

$node->stop;
done_testing();
