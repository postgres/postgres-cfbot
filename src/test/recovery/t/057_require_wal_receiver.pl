# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for the require_wal_receiver parameter with target_session_attrs.
#
# The parameter rejects a standby that has no live WAL receiver process
# (pg_stat_wal_receiver returns no row).

use strict;
use warnings;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

# Primary
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$primary->start;

# create a user to test the feature without superuser rights
$primary->safe_psql('postgres',
	"CREATE ROLE regress_walrcv_user LOGIN");

# standby_live: normal streaming standby.
$primary->backup('backup_live');
my $standby_live = PostgreSQL::Test::Cluster->new('standby_live');
$standby_live->init_from_backup($primary, 'backup_live', has_streaming => 1);
$standby_live->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_live->append_conf('postgresql.conf', "wal_retrieve_retry_interval = '60s'");
$standby_live->start;

# standby_norecv: in recovery but with no primary_conninfo
$primary->backup('backup_norecv');
my $standby_norecv = PostgreSQL::Test::Cluster->new('standby_norecv');
$standby_norecv->init_from_backup($primary, 'backup_norecv');
$standby_norecv->append_conf('postgresql.conf', "listen_addresses = 'localhost'");
$standby_norecv->set_standby_mode();
$standby_norecv->start;

# make sure standby_live is actually streaming before we rely on it.
$standby_live->poll_query_until('postgres',
	"SELECT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)")
	or die "standby_live never started a WAL receiver";

# make sure standby_norecv is in recovery but not streaming.
is( $standby_norecv->safe_psql('postgres', 'SELECT pg_is_in_recovery()'),
	't',
	'standby_norecv is in recovery');

is( $standby_norecv->safe_psql('postgres',
		'SELECT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)'),
	'f',
	'standby_norecv has no WAL receiver');

my ($stdout, $stderr);
my $port_primary       = $primary->port;
my $port_live          = $standby_live->port;
my $port_norecv        = $standby_norecv->port;

# streaming standby is accepted
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live, "streaming standby accepted");

# standby with no WAL receiver is rejected
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby without WAL receiver rejected");

# without require_wal_receiver, the dead standby IS accepted
$standby_norecv->psql(
	'postgres',
	'SELECT current_setting(\'port\')',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"dead standby accepted when require_wal_receiver is omitted");

# with require_wal_receiver disabled, the dead standby is accepted
$standby_norecv->psql(
	'postgres',
	'SELECT current_setting(\'port\')',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby require_wal_receiver=0",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"dead standby accepted when require_wal_receiver is set to 0");

# prefer-standby: dead standby skipped, falls back to primary.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_primary,$port_norecv "
	  . "target_session_attrs=prefer-standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"prefer-standby: skips dead standby, connects to primary");

# prefer-standby: when no host passes the check, the fallback pass accepts
# any server, including a standby with no WAL receiver.  This is what
# distinguishes prefer-standby from "any", which fails outright above.
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=prefer-standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"prefer-standby: fallback pass accepts a standby with no WAL receiver");

# any: dead standby skipped, connects to primary.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_primary "
	  . "target_session_attrs=any require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"any: skips dead standby, connects to primary");

# any: dead standby skipped, live standby accepted.
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_live "
	  . "target_session_attrs=any require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live,
	"any: skips dead standby, connects to live standby");

# any: connection fails if there are only standby servers in the
# connection string and they have no WAL receiver running
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost "
	  . "port=$port_norecv "
	  . "target_session_attrs=any require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"any: with no live standby fails");

# read-only: dead standby skipped, live standby accepted (primary is
# read-write so skipped by the read-only filter).
$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost,localhost "
	  . "port=$port_primary,$port_norecv,$port_live "
	  . "target_session_attrs=read-only require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live,
	"read-only: skips dead standby, connects to live standby");

# standby only, all standbys dead: connection fails.
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby-only with no live standby fails");

# read-write: check is bypassed entirely; connects to primary even when a
# dead standby is listed first.
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost,localhost "
	  . "port=$port_norecv,$port_primary "
	  . "target_session_attrs=read-write require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"read-write: check bypassed, connects to primary");

# primary: connecting directly to a primary with the check enabled is fine
# (pg_is_in_recovery() is false, so require_wal_receiver is ignored).
$primary->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_primary "
	  . "require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_primary,
	"primary accepted (require_wal_receiver is ignored)");

# invalid value is rejected during option parsing.
$standby_live->psql(
	'postgres',
	'SELECT 1',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "require_wal_receiver=foo",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/invalid require_wal_receiver value: "foo"/,
	"invalid boolean value rejected");

# non-superuser: a live standby is accepted.  pg_stat_wal_receiver exposes
# the walreceiver pid (hence a row) even to unprivileged roles, while
# masking every other column.  The check uses EXISTS, so it must work
# without pg_read_all_stats.  We probe with current_setting('port')
# rather than inet_server_port(), so the query itself needs no special
# privilege.
$standby_live->psql(
	'postgres',
	"SELECT current_setting('port')",
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "user=regress_walrcv_user "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_live, "non-superuser: live standby accepted");

# non-superuser: a standby with no WAL receiver is still rejected.
$standby_norecv->psql(
	'postgres',
	"SELECT current_setting('port')",
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "user=regress_walrcv_user "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"non-superuser: standby without WAL receiver rejected");

# Replication connections bypass the check entirely: a physical replication
# connection cannot execute SQL at all, so applying the check would break
# every one of them (including a cascading standby's own WAL receiver).  Use
# the standby with no WAL receiver, which would be rejected on an ordinary
# connection, and SHOW, which a WAL sender does accept.
$standby_norecv->psql(
	'postgres',
	'SHOW port',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby replication=1 require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"physical replication connection bypasses the check");

# "true" rather than "1": this is the spelling the WAL receiver itself and
# pg_basebackup use, so it is the one that matters in practice.
$standby_norecv->psql(
	'postgres',
	'SHOW port',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby replication=true "
	  . "require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"replication=true bypasses the check");

# The same for a logical replication connection.
$standby_norecv->psql(
	'postgres',
	'SHOW port',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby replication=database "
	  . "require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
is($stdout, $port_norecv,
	"logical replication connection bypasses the check");

# An explicitly false "replication" value is an ordinary connection, so the
# check still applies.
$standby_norecv->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_norecv "
	  . "target_session_attrs=standby replication=off require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"replication=off is an ordinary connection, check applies");

{
	# PGREQUIREWALRECEIVER environment variable enabled
	local $ENV{PGREQUIREWALRECEIVER} = '1';
	$standby_norecv->psql(
		'postgres',
		'SELECT 1',
		connstr => "dbname=postgres host=localhost port=$port_norecv "
			. "target_session_attrs=standby",
		stdout  => \$stdout,
		stderr  => \$stderr,
	);
	like($stderr, qr/standby has no active WAL receiver/,
		"PGREQUIREWALRECEIVER set to 1");

	# PGREQUIREWALRECEIVER environment variable override via require_wal_receiver
	$standby_norecv->psql(
		'postgres',
		'SELECT inet_server_port()',
		connstr => "dbname=postgres host=localhost port=$port_norecv "
			. "target_session_attrs=standby require_wal_receiver=0",
		stdout  => \$stdout,
		stderr  => \$stderr,
	);
	is($stdout, $port_norecv,
		"PGREQUIREWALRECEIVER override via require_wal_receiver");

	# PGREQUIREWALRECEIVER environment variable disabled
	local $ENV{PGREQUIREWALRECEIVER} = '0';
	$standby_norecv->psql(
		'postgres',
		'SELECT inet_server_port()',
		connstr => "dbname=postgres host=localhost port=$port_norecv "
			. "target_session_attrs=standby",
		stdout  => \$stdout,
		stderr  => \$stderr,
	);
	is($stdout, $port_norecv,
		"PGREQUIREWALRECEIVER set to 0");
}

# upstream lost: after the primary stops, standby_live's WAL receiver exits
# and (thanks to the long retry interval) stays gone, so the host is
# rejected.  Run last because it stops the primary.
$primary->stop;
$standby_live->poll_query_until('postgres',
	"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_wal_receiver)")
	or die "standby_live WAL receiver did not exit after primary stop";

$standby_live->psql(
	'postgres',
	'SELECT inet_server_port()',
	connstr => "dbname=postgres host=localhost port=$port_live "
	  . "target_session_attrs=standby require_wal_receiver=1",
	stdout  => \$stdout,
	stderr  => \$stderr,
);
like($stderr, qr/standby has no active WAL receiver/,
	"standby rejected after losing upstream");

done_testing();
