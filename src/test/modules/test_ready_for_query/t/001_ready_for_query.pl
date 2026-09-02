# Copyright (c) 2026, PostgreSQL Global Development Group

# Test ReadyForQuery wire protocol extension and hook

use strict;
use warnings;

# Set a hard test-level timeout of 60 seconds to detect and handle any hanging condition
$SIG{ALRM} = sub { die "Test timed out after 60 seconds\n"; };
alarm(60);

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_ready_for_query'\n"
);
$node->start;

# 1. Test SQL-level GUC settings and extension management
my $guc_val = $node->safe_psql('postgres', 'SHOW ready_for_query_message;');
is($guc_val, 'plain', 'default GUC value is plain');

$guc_val = $node->safe_psql('postgres', "SET ready_for_query_message = 'rich'; SHOW ready_for_query_message;");
is($guc_val, 'rich', 'GUC can be set to rich');

$guc_val = $node->safe_psql('postgres', "SET ready_for_query_message = 'plain'; SHOW ready_for_query_message;");
is($guc_val, 'plain', 'GUC can be reset to plain');

my ($ret, $stdout, $stderr) = $node->psql('postgres', "SET ready_for_query_message = 'invalid';");
isnt($ret, 0, 'invalid GUC value rejected');
like($stderr, qr/invalid value for parameter "ready_for_query_message"/, 'error message on invalid GUC value');

$node->safe_psql('postgres', 'CREATE EXTENSION test_ready_for_query;');
$node->safe_psql('postgres', 'DROP EXTENSION test_ready_for_query;');

# Helper function to read exactly $n bytes from a socket
sub read_exact
{
	my ($sock, $len) = @_;
	my $buf = '';
	while (length($buf) < $len)
	{
		my $chunk;
		my $n = $sock->sysread($chunk, $len - length($buf));
		die "unexpected EOF on socket" if !defined($n) || $n == 0;
		$buf .= $chunk;
	}
	return $buf;
}

# Helper function to send simple query 'Q' message
sub send_query
{
	my ($sock, $sql) = @_;
	my $payload = $sql . "\0";
	my $len = 4 + length($payload);
	my $msg = 'Q' . pack('N', $len) . $payload;
	$sock->syswrite($msg);
}

# Helper function to read messages until ReadyForQuery ('Z')
# Returns ($mlen, $tx_status, \%kv, \@other_msgs)
sub read_until_ready_for_query
{
	my ($sock) = @_;
	my @other_msgs = ();
	while (1)
	{
		my $mtype = read_exact($sock, 1);
		my $raw_len = read_exact($sock, 4);
		my $mlen = unpack('N', $raw_len);
		my $body_len = $mlen - 4;
		my $body = $body_len > 0 ? read_exact($sock, $body_len) : '';

		if ($mtype eq 'Z')
		{
			my $tx_status = substr($body, 0, 1);
			my %kv = ();
			my $offset = 1;
			while ($offset < length($body))
			{
				my $klen = ord(substr($body, $offset++, 1));
				my $key = substr($body, $offset, $klen);
				$offset += $klen;
				my $vlen = ord(substr($body, $offset++, 1));
				my $val = $vlen > 0 ? substr($body, $offset, $vlen) : '';
				$offset += $vlen;
				$kv{$key} = $val;
			}
			return ($mlen, $tx_status, \%kv, \@other_msgs);
		}
		else
		{
			push @other_msgs, { type => $mtype, body => $body };
		}
	}
}

if (!$node->raw_connect_works())
{
	plan skip_all => "this test requires working raw_connect()";
}

# 2. Connect via raw socket and perform StartupPacket
my $dbuser = $node->safe_psql('postgres', 'SELECT current_user;');
my $sock = $node->raw_connect();

my $startup_body = pack('N', 196608) . "user\0$dbuser\0database\0postgres\0\0";
my $startup_msg = pack('N', 4 + length($startup_body)) . $startup_body;
$sock->syswrite($startup_msg);

# Read initial ReadyForQuery (default 'plain' mode)
my ($len1, $tx1, $kv1, $msgs1) = read_until_ready_for_query($sock);
is($len1, 5, "default ReadyForQuery message length is 5 (plain mode)");
is($tx1, 'I', "initial transaction status is Idle ('I')");
is(scalar(keys %$kv1), 0, "no extra key-value pairs in plain mode");

# Verify plain mode does not send ParameterStatus for ready_for_query_message
my @param_rfq = grep {
	$_->{type} eq 'S' && $_->{body} =~ /^ready_for_query_message\0/
} @$msgs1;
is(scalar(@param_rfq), 0, "plain mode sends no ParameterStatus for ready_for_query_message");

# 3. Switch to 'rich' mode
send_query($sock, "SET ready_for_query_message = 'rich';");
my ($len2, $tx2, $kv2) = read_until_ready_for_query($sock);
cmp_ok($len2, '>', 5, "rich mode ReadyForQuery message length > 5");
is($tx2, 'I', "transaction status is Idle ('I')");
is($kv2->{'T'}, '0', "T=0 when no temp tables exist");
is($kv2->{'H'}, '0', "H=0 when no with-hold cursors exist");
is($kv2->{'P'}, '0', "P=0 when no prepared statements exist");
ok(defined $kv2->{'l'}, "l (binary LSN) key is present in rich mode");
is(length($kv2->{'l'}), 8, "l key value is 8 bytes");

# 4. Temporary table lifecycle: CREATE -> DROP -> DISCARD TEMP -> DISCARD ALL
send_query($sock, "CREATE TEMP TABLE t_temp (id int);");
my ($len3, $tx3, $kv3) = read_until_ready_for_query($sock);
is($kv3->{'T'}, '1', "T=1 after temporary table creation");
is($kv3->{'temp_tables_info'}, 'has_temp_namespace', "hook injected temp_tables_info key");

# DROP TABLE resets T to 0
send_query($sock, "DROP TABLE t_temp;");
my ($len_drop, $tx_drop, $kv_drop) = read_until_ready_for_query($sock);
is($kv_drop->{'T'}, '0', "T=0 after DROP TABLE");
ok(!exists $kv_drop->{'temp_tables_info'}, "temp_tables_info gone after DROP TABLE");

# Re-create temp table, then DISCARD TEMP resets T to 0
send_query($sock, "CREATE TEMP TABLE t_temp2 (id int);");
my ($len_dt1, $tx_dt1, $kv_dt1) = read_until_ready_for_query($sock);
is($kv_dt1->{'T'}, '1', "T=1 after re-creating temp table");

send_query($sock, "DISCARD TEMP;");
my ($len_dt2, $tx_dt2, $kv_dt2) = read_until_ready_for_query($sock);
is($kv_dt2->{'T'}, '0', "T=0 after DISCARD TEMP");

# Re-create temp table, then DISCARD ALL resets T to 0
send_query($sock, "CREATE TEMP TABLE t_temp3 (id int);");
my ($len_da1, $tx_da1, $kv_da1) = read_until_ready_for_query($sock);
is($kv_da1->{'T'}, '1', "T=1 after third temp table");

send_query($sock, "DISCARD ALL;");
read_until_ready_for_query($sock);

send_query($sock, "SET ready_for_query_message = 'rich';");
my ($len_da2, $tx_da2, $kv_da2) = read_until_ready_for_query($sock);
is($kv_da2->{'T'}, '0', "T=0 after DISCARD ALL");

# 5. Cursors: verify 100 cursors without hold do not set H, and WITH HOLD cursor sets H
my $c100_sql = "BEGIN; " . join(" ", map { "DECLARE c$_ CURSOR FOR SELECT 1;" } (1..100));
send_query($sock, $c100_sql);
my ($len_c100, $tx_c100, $kv_c100) = read_until_ready_for_query($sock);
is($kv_c100->{'H'}, '0', "H=0 with 100 cursors without HOLD inside transaction");
send_query($sock, "ROLLBACK;");
read_until_ready_for_query($sock);

# Create WITH HOLD cursor and verify 'H' and commit LSN 'l'
send_query($sock, "BEGIN; DECLARE cur CURSOR WITH HOLD FOR SELECT 1; COMMIT;");
my ($len4, $tx4, $kv4) = read_until_ready_for_query($sock);
is($kv4->{'H'}, '1', "H=1 after creating cursor WITH HOLD");
my $lsn64 = unpack('Q>', $kv4->{'l'});
cmp_ok($lsn64, '>', 0, "binary LSN is non-zero after COMMIT");

# Close cursor resets H to 0
send_query($sock, "CLOSE cur;");
my ($len_close, $tx_close, $kv_close) = read_until_ready_for_query($sock);
is($kv_close->{'H'}, '0', "H=0 after CLOSE cur");

# 6. Prepared statements 'P'
send_query($sock, "PREPARE p1 AS SELECT 1;");
my ($len_prep, $tx_prep, $kv_prep) = read_until_ready_for_query($sock);
is($kv_prep->{'P'}, '1', "P=1 after PREPARE");

send_query($sock, "DEALLOCATE p1;");
my ($len_dealloc, $tx_dealloc, $kv_dealloc) = read_until_ready_for_query($sock);
is($kv_dealloc->{'P'}, '0', "P=0 after DEALLOCATE");

send_query($sock, "PREPARE p2 AS SELECT 2;");
my ($len_p2, $tx_p2, $kv_p2) = read_until_ready_for_query($sock);
is($kv_p2->{'P'}, '1', "P=1 after PREPARE p2");

send_query($sock, "DEALLOCATE ALL;");
my ($len_dp, $tx_dp, $kv_dp) = read_until_ready_for_query($sock);
is($kv_dp->{'P'}, '0', "P=0 after DEALLOCATE ALL");

# 7. Switch back to 'plain' mode
send_query($sock, "SET ready_for_query_message = 'plain';");
my ($len5, $tx5, $kv5) = read_until_ready_for_query($sock);
is($len5, 5, "reverting to plain mode restores ReadyForQuery length to 5");
is(scalar(keys %$kv5), 0, "no extra key-value pairs in plain mode");

# 8. Test with StartupPacket containing options="-c ready_for_query_message=rich"
my $sock2 = $node->raw_connect();
my $startup_body2 = pack('N', 196608) . "user\0$dbuser\0database\0postgres\0options\0-c ready_for_query_message=rich\0\0";
my $startup_msg2 = pack('N', 4 + length($startup_body2)) . $startup_body2;
$sock2->syswrite($startup_msg2);

my ($len_opt, $tx_opt, $kv_opt) = read_until_ready_for_query($sock2);
cmp_ok($len_opt, '>', 5, "startup option enables rich mode on initial ReadyForQuery");
is($kv_opt->{'T'}, '0', "T=0 on fresh session");
is($kv_opt->{'H'}, '0', "H=0 on fresh session");
is($kv_opt->{'P'}, '0', "P=0 on fresh session");

# 9. Test mid-session GUC assign hook: set rich after temp table was already created
my $sock3 = $node->raw_connect();
my $startup_body3 = pack('N', 196608) . "user\0$dbuser\0database\0postgres\0\0";
my $startup_msg3 = pack('N', 4 + length($startup_body3)) . $startup_body3;
$sock3->syswrite($startup_msg3);
read_until_ready_for_query($sock3);

# Create temp table in plain mode
send_query($sock3, "CREATE TEMP TABLE t_preexisting (id int);");
my ($len_pre, $tx_pre, $kv_pre) = read_until_ready_for_query($sock3);
is($len_pre, 5, "plain mode RFQ after temp table creation");

# Switch to rich mode: GUC assign hook initializes session_has_temp_tables
send_query($sock3, "SET ready_for_query_message = 'rich';");
my ($len_hook, $tx_hook, $kv_hook) = read_until_ready_for_query($sock3);
cmp_ok($len_hook, '>', 5, "switched to rich mode");
is($kv_hook->{'T'}, '1', "T=1 detected by GUC assign hook for pre-existing temp table");

send_query($sock3, "DROP TABLE t_preexisting;");
my ($len_post, $tx_post, $kv_post) = read_until_ready_for_query($sock3);
is($kv_post->{'T'}, '0', "T=0 after dropping pre-existing temp table");

close($sock);
close($sock2);
close($sock3);
$node->stop;

alarm(0);
done_testing();
