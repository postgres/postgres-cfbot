# Copyright (c) 2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

plan skip_all => 'sending SIGINT on Windows terminates the test itself'
  if $windows_os;
plan skip_all => 'IO::Pty is needed to run this test'
  unless eval { require IO::Pty; 1 };

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;

my $hba = $node->data_dir . '/pg_hba.conf';
my $old_hba = slurp_file($hba);
open(my $hba_fh, '>', $hba) or die "open $hba: $!";
print $hba_fh "local all interrupt_user scram-sha-256\n";
print $hba_fh $old_hba;
close($hba_fh) or die "close $hba: $!";

$node->start;
$node->safe_psql('postgres',
	'CREATE TABLE copy_log (id int); CREATE ROLE interrupt_user LOGIN');

my $inner = $node->basedir . '/inner.sql';
my $outer = $node->basedir . '/outer.sql';
append_to_file($inner, "SELECT 111;\n");
append_to_file($outer, "\\i $inner \\prompt 'OUTER: ' answer\n");
my $password_script = $node->basedir . '/password_then_prompt.sql';
append_to_file($password_script,
	"\\password interrupt_user \\prompt 'AFTER: ' answer\n");

# simple_prompt_extended() can write to /dev/tty or stderr.  Accept either
# stream, and discard the matched output before issuing the next command.
sub query_until
{
	my ($h, $until, $query) = @_;
	$h->{timeout}->start();
	$h->{stdin} .= $query;
	$h->{run}->pump() until ($h->{stdout} . $h->{stderr}) =~ $until;
	my $output = $h->{stdout} . $h->{stderr};
	$h->{stdout} = '';
	$h->{stderr} = '';
	return $output;
}

my $ready = qr/ready> /;

for my $no_readline (1, 0)
{
	next
	  if !$no_readline
	  && ((!defined($ENV{with_readline}) || $ENV{with_readline} ne 'yes')
		|| defined($ENV{SKIP_READLINE_TESTS}));

	my $mode = $no_readline ? 'without readline' : 'with readline';
	my $h = $node->interactive_psql('postgres',
		extra_params => $no_readline ? ['-n'] : []);
	query_until(
		$h,
		qr/interrupt-ready\r?\nready> /,
		"\\set PROMPT1 'ready> '\n\\echo interrupt-ready\n");

	# After the inner MainLoop returns, Ctrl-C during \prompt on the same
	# line must use the context saved by sigsetjmp in the outer MainLoop.
	query_until($h, qr/OUTER: /, "\\i $outer\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	like(query_until($h, $ready, "SELECT 222;\n"),
		qr/\r?\n222\r?\n/,
		"canceling a prompt after nested \\i preserves the session, $mode");

	# Password input must not overwrite MainLoop's sigjmp_buf either.  A
	# later \prompt on the same line can set sigint_interrupt_enabled to true
	# before MainLoop calls sigsetjmp again.
	query_until(
		$h,
		qr/Enter new password for user "interrupt_user": /,
		"\\i $password_script\n");
	query_until($h, qr/Enter it again: /, "test-password\n");
	query_until($h, qr/AFTER: /, "test-password\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	like(
		query_until($h, $ready, "SELECT 223;\n"),
		qr/\r?\n223\r?\n/,
		"canceling a prompt after password input preserves the session, $mode"
	);

	# COPY must handle the interrupt itself and finish the COPY protocol,
	# rather than jumping directly to MainLoop.
	$node->safe_psql('postgres', 'TRUNCATE copy_log');
	query_until($h, qr/>> /, "COPY copy_log FROM STDIN;\n");
	query_until($h, qr/>> /, "1\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	is($node->safe_psql('postgres', 'SELECT count(*) FROM copy_log'),
		'0', "canceling COPY rolls back its rows, $mode");
	like(query_until($h, $ready, "SELECT 333;\n"),
		qr/\r?\n333\r?\n/,
		"connection remains usable after canceling COPY, $mode");

	query_until($h, qr/>> /, "COPY copy_log FROM STDIN (FORMAT binary);\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	like(query_until($h, $ready, "SELECT 444;\n"),
		qr/\r?\n444\r?\n/,
		"connection remains usable after canceling binary COPY, $mode");

	query_until($h, qr/>> /, "COPY copy_log FROM STDIN;\n");
	query_until($h, $ready, "2\n\\.\n");
	is($node->safe_psql('postgres', 'SELECT id FROM copy_log'),
		'2', "COPY can subsequently complete, $mode");

	query_until($h, qr/VALUE: /, "\\prompt 'VALUE: ' answer\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	query_until($h, qr/VALUE: /, "\\prompt 'VALUE: ' answer\n");
	query_until($h, $ready, "prompt-ok\n");
	like(query_until($h, $ready, "\\echo :answer\n"),
		qr/prompt-ok/,
		"interactive prompt can be retried after cancellation, $mode");

	$node->safe_psql('postgres', 'ALTER ROLE interrupt_user PASSWORD NULL');
	for my $second_prompt (0, 1)
	{
		query_until(
			$h,
			qr/Enter new password for user "interrupt_user": /,
			"\\password interrupt_user\n");
		query_until($h, qr/Enter it again: /, "test-password\n")
		  if $second_prompt;
		$h->{run}->signal('INT');
		query_until($h, $ready, '');
		is( $node->safe_psql(
				'postgres',
				q{SELECT rolpassword IS NULL FROM pg_authid WHERE rolname = 'interrupt_user'}
			),
			't',
			"canceling password prompt $second_prompt leaves password unchanged, $mode"
		);
	}
	query_until(
		$h,
		qr/Enter new password for user "interrupt_user": /,
		"\\password interrupt_user\n");
	query_until($h, qr/Enter it again: /, "test-password\n");
	query_until($h, $ready, "test-password\n");
	is( $node->safe_psql(
			'postgres',
			q{SELECT rolpassword IS NOT NULL FROM pg_authid WHERE rolname = 'interrupt_user'}
		),
		't',
		"password change succeeds after cancellation, $mode");

	query_until(
		$h,
		qr/Password for user interrupt_user: /,
		"\\connect postgres interrupt_user\n");
	$h->{run}->signal('INT');
	query_until($h, $ready, '');
	like(query_until($h, $ready, "SELECT 555;\n"),
		qr/\r?\n555\r?\n/,
		"canceling connection password preserves the old connection, $mode");
	query_until(
		$h,
		qr/Password for user interrupt_user: /,
		"\\connect postgres interrupt_user\n");
	query_until($h, $ready, "test-password\n");
	like(
		query_until($h, $ready, "SELECT current_user;\n"),
		qr/\r?\ninterrupt_user\r?\n/,
		"password connection succeeds after cancellation, $mode");

	ok($h->quit, "psql exits normally, $mode");
}

$node->stop;
done_testing();
