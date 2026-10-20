# Copyright (c) 2026, PostgreSQL Global Development Group
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IO::Socket::INET;
use IO::Select;
use IPC::Run qw(run);
use Time::HiRes qw(time sleep);

plan skip_all => 'Windows socket inheritance test' unless $windows_os;
my $helper = $ENV{PG_CTL_SOCKET_HELPER};
die 'PG_CTL_SOCKET_HELPER is required' unless defined $helper && -f $helper;
$helper =~ s!\\!/!g;
my $node = PostgreSQL::Test::Cluster->new('socket_inheritance');
my $dir = PostgreSQL::Test::Utils::tempdir;
my $ready = "$dir/copy.ready";
my $release = "$dir/copy.release";
my (@identity, $socket);

sub helper_exit
{
	my (@args) = @_;
	my ($stdout, $stderr);
	run [ $helper, @args ], '>', \$stdout, '2>', \$stderr;
	return $? >> 8;
}

# Release only our bounded helper. Cluster cleanup knows the owned postmaster.
END
{
	if (defined $release && -d $dir)
	{
		open my $f, '>', $release or die $!;
		close $f;
	}
	helper_exit('--wait', @identity) if @identity;
	close $socket if defined $socket;
	$node->stop('immediate') if defined $node;
}

sub read_exact
{
	my ($count) = @_;
	my $bytes = '';
	while (length($bytes) < $count)
	{
		die 'startup socket read timed out'
		  unless IO::Select->new($socket)->can_read(10);
		my $n = sysread($socket, my $part, $count - length($bytes));
		die "startup socket read: $!" unless defined $n && $n;
		$bytes .= $part;
	}
	return $bytes;
}

sub write_packet
{
	my ($bytes) = @_;
	while (length($bytes))
	{
		my $n = syswrite($socket, $bytes);
		die "socket write: $!" unless defined $n && $n;
		substr($bytes, 0, $n, '');
	}
}

sub transport_closed
{
	my $deadline = time + 1;
	my $select = IO::Select->new($socket);
	while (time < $deadline)
	{
		return 0 unless $select->can_read($deadline - time);
		my $n = sysread($socket, my $bytes, 65536);
		return 1 if defined $n && $n == 0;
		return 1 if !defined $n && $!{ECONNRESET};
		die "socket read: $!" unless defined $n;
	}
	return 0;
}

$node->init(force_initdb => 1);
$node->append_conf('postgresql.conf', "listen_addresses = '127.0.0.1'");
# The raw test client does not implement the default Windows SSPI exchange.
# Change only this disposable node's loopback authentication rule.
my $hba = $node->data_dir . '/pg_hba.conf';
my $hba_contents = slurp_file($hba);
open my $auth, '>', $hba or die $!;
print $auth "host all all 127.0.0.1/32 trust\n", $hba_contents;
close $auth;
$node->start;
$socket = IO::Socket::INET->new(PeerAddr => '127.0.0.1',
	PeerPort => $node->port, Proto => 'tcp', Timeout => 10) or die $!;
my $user = $node->safe_psql('postgres', 'SELECT current_user');
$user =~ s/[\r\n]+\z//;
my $body = pack('N', 196608) . "user\0$user\0database\0postgres\0\0";
write_packet(pack('N', length($body) + 4) . $body);
while (1)
{
	my $type = read_exact(1);
	my $length = unpack('N', read_exact(4));
	my $message = read_exact($length - 4);
	die "startup error: $message" if $type eq 'E';
	die 'raw client requires trust authentication'
	  if $type eq 'R' && unpack('N', $message) != 0;
	last if $type eq 'Z';
}
my $program = qq("$helper" "$ready" "$release");
$program =~ s/'/''/g;
my $query = "COPY (SELECT 1) TO PROGRAM '$program'\0";
write_packet('Q' . pack('N', length($query) + 4) . $query);
my $deadline = time + 10;
while (time < $deadline)
{
	if (-f $ready)
	{
		my $identity = slurp_file($ready);
		@identity = ($identity =~ /^(\d+) (\d+) (\d+)\n$/);
		last if @identity;
	}
	sleep 0.05;
}
ok(scalar(@identity) == 3, 'COPY helper publishes exact PID and creation time');
die 'helper did not become ready' unless @identity;
is(helper_exit('--alive', @identity), 0, 'helper alive before shutdown');
$node->stop('immediate');
ok(!-f $node->data_dir . '/postmaster.pid', 'owned postmaster stopped');
my $survived = helper_exit('--alive', @identity) == 0;
ok($survived, 'precondition: COPY helper remains alive after shutdown');
SKIP:
{
	skip 'environment terminated the COPY helper, socket lifetime not tested', 2
	  unless $survived;
	ok(transport_closed(), 'backend client closes while COPY helper is alive');
	is(helper_exit('--bind', $node->port), 0,
		'listener port can be rebound while COPY helper is alive');
}
append_to_file($release, 'release');
is(helper_exit('--wait', @identity), 0, 'exact helper exits after release');
$deadline = time + 5;
my $bound = 1;
while (time < $deadline)
{
	$bound = helper_exit('--bind', $node->port);
	last if $bound == 0;
	sleep 0.05;
}
is($bound, 0, 'port reusable after helper and COPY shell exit');
ok(transport_closed(), 'client transport closed after helper release');
done_testing();
