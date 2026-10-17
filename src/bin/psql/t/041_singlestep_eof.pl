# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'IO::Pty is needed to run this test'
  unless eval { require IO::Pty; 1 };
plan skip_all => 'terminal EOF input is not supported by this test on Windows'
  if $windows_os;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;
$node->safe_psql('postgres', 'CREATE TABLE step_log (id int)');

my $multiline = $node->basedir . '/multiline.sql';
append_to_file($multiline,
	"INSERT INTO step_log VALUES (1);\nINSERT INTO step_log VALUES (2);\n");
my $same_line = $node->basedir . '/same_line.sql';
append_to_file($same_line,
	"INSERT INTO step_log VALUES (1); \\set eof_marker changed \\echo :eof_marker\n"
	  . "INSERT INTO step_log VALUES (2);\n");
my $send_query = $node->basedir . '/send_query.sql';
append_to_file($send_query,
		"INSERT INTO step_log VALUES (1) \\g \\set eof_marker changed\n"
	  . "INSERT INTO step_log VALUES (2);\n");
my $nested = $node->basedir . '/nested.sql';
append_to_file($nested,
		"\\i $same_line \\set eof_marker changed\n"
	  . "INSERT INTO step_log VALUES (4);\n");

my $confirm = qr/return to cancel[^\r\n]*\r?\n/;
my $ready = qr/postgres=# /;

for my $no_readline (1, 0)
{
	next
	  if !$no_readline
	  && ((!defined($ENV{with_readline}) || $ENV{with_readline} ne 'yes')
		|| defined($ENV{SKIP_READLINE_TESTS}));

	my $mode = $no_readline ? 'without readline' : 'with readline';
	my $h = $node->interactive_psql('postgres',
		extra_params => $no_readline ? ['-n'] : []);
	$h->set_query_timer_restart;
	$h->query_until(qr/eof-ready\r?\npostgres=# /, "\\echo eof-ready\n");

	for my $on_error_stop ('off', 'on')
	{
		$h->query_until($ready, "\\set ON_ERROR_STOP $on_error_stop\n");
		for my $case (
			[ 'multiple lines', $multiline ],
			[ 'multiple commands on one line', $same_line ],
			[ 'query sent by \\g', $send_query ],
			[ 'nested scripts', $nested ])
		{
			my ($name, $script) = @$case;
			my $description = "$name, ON_ERROR_STOP $on_error_stop, $mode";
			$node->safe_psql('postgres', 'TRUNCATE step_log');
			$h->query_until(
				qr/eof-setup\r?\npostgres=# /,
				"\\set SINGLESTEP on\n\\set eof_marker unchanged\n\\echo eof-setup\n"
			);
			$h->query_until($confirm, "\\i $script\n");
			my $output = $h->query_until($ready, "\004");
			if ($script eq $same_line)
			{
				unlike(
					$output,
					qr/(?:^|\r?\n)(?:unchanged|changed)\r?\n/,
					"Ctrl-D does not execute later \\echo: $description");
			}
			is( $node->safe_psql('postgres', 'SELECT count(*) FROM step_log'),
				'0',
				"Ctrl-D does not execute SQL: $description");
			like(
				$h->query_until($ready, "\\echo :eof_marker\n"),
				qr/\r?\nunchanged\r?\n/,
				"Ctrl-D does not execute later \\set: $description");
		}
	}

	# A later script must still wait for confirmation before each query.
	$h->query_until($ready, "\\set ON_ERROR_STOP off\n");
	$h->query_until($confirm, "\\i $multiline\n");
	$h->query_until($confirm, "\n");
	$h->query_until($ready, "\n");
	is( $node->safe_psql(
			'postgres', 'SELECT array_agg(id ORDER BY id) FROM step_log'),
		'{1,2}',
		"a later script accepts confirmation for each query, $mode");

	# Unlike Ctrl-D, x skips just one query, not the rest of the script.
	$node->safe_psql('postgres', 'TRUNCATE step_log');
	$h->query_until($confirm, "\\i $multiline\n");
	$h->query_until($confirm, "x\n");
	$h->query_until($ready, "\n");
	is($node->safe_psql('postgres', 'SELECT id FROM step_log'),
		'2', "x still skips only the current query, $mode");

	# The same cancellation must work for SQL entered interactively.
	$node->safe_psql('postgres', 'TRUNCATE step_log');
	$h->query_until($confirm,
		"INSERT INTO step_log VALUES (5); INSERT INTO step_log VALUES (6);\n"
	);
	$h->query_until($ready, "\004");
	is($node->safe_psql('postgres', 'SELECT count(*) FROM step_log'),
		'0', "Ctrl-D cancels the rest of an interactive SQL line, $mode");
	$h->query_until($confirm, "SELECT 777;\n");
	like($h->query_until($ready, "\n"),
		qr/\r?\n777\r?\n/, "the interactive session remains usable, $mode");

	ok($h->quit, "psql exits normally, $mode");

	# EOF while reading a command must still end the interactive session.
	my $exit_session = $node->interactive_psql('postgres',
		extra_params => $no_readline ? ['-n'] : []);
	$exit_session->set_query_timer_restart;
	$exit_session->query_until(qr/eof-exit\r?\npostgres=# /,
		"\\echo eof-exit\n");
	$exit_session->{stdin} .= "\004";
	ok($exit_session->{run}->finish,
		"Ctrl-D at the command prompt still exits psql, $mode");
}

$node->stop;
done_testing();
