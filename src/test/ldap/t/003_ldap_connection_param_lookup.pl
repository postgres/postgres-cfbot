
# Copyright (c) 2025-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use FindBin;
use lib "$FindBin::RealBin/..";

use File::Copy;
use LdapServer;
use PostgreSQL::Test::Utils;
use PostgreSQL::Test::Cluster;
use Test::More;

if ($ENV{with_ldap} ne 'yes')
{
	plan skip_all => 'LDAP not supported by this build';
}
elsif (!$ENV{PG_TEST_EXTRA} || $ENV{PG_TEST_EXTRA} !~ /\bldap\b/)
{
	plan skip_all =>
	  'Potentially unsafe test LDAP not enabled in PG_TEST_EXTRA';
}
elsif (!$LdapServer::setup)
{
	plan skip_all => $LdapServer::setup_error;
}

# This tests scenarios related to the service name and the service file,
# for the connection options and their environment variables.
my $dummy_node = PostgreSQL::Test::Cluster->new('dummy_node');
$dummy_node->init;

my $node = PostgreSQL::Test::Cluster->new('node');
$node->init;
$node->start;

note "setting up LDAP server";

my $ldap_rootpw = 'secret';
my $ldap = LdapServer->new($ldap_rootpw, 'anonymous');    # use anonymous auth
$ldap->ldapadd_file('authdata.ldif');
$ldap->ldapsetpw('uid=test1,dc=example,dc=net', 'secret1');
$ldap->ldapsetpw('uid=test2,dc=example,dc=net', 'secret2');

my $td = PostgreSQL::Test::Utils::tempdir;

# create ldap file based on postgres connection info
my $ldif_valid = "$td/connection_params.ldif";
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=mydatabase,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:mydatabase
description:application_name=appname_from_ldap_mydatabase
description:host=} . $node->host . qq{
description:port=} . $node->port . qq{
});

# Valid ldap record with geqo and application_name set
# to test how parameters are set/overridden
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=mydatabasegeqooff,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:mydatabasegeqooff
description:application_name=appname_from_ldap_lookup_geqo_off
description:options=--geqo=off
});

# Invalid LDAP record with no entries
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=noentries,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:noentries
});

# Invalid LDAP record with missing = after application_name
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=missingequal,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:missingequal
description:application_name
});

# Invalid LDAP record with non existent parameter
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=invalidconnoptionservice,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:invalidconnoptionservice
description:invalidconnoption=1
});

# Valid LDAP record which has ldapserviceurl parameter
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=ldapurlinldapurl,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:ldapurlinldapurl
description:ldapserviceurl=ldap://localhost:1234/dc=example,dc=net?description?one?(cn=mydatabasegeqooff)
description:application_name=appname_from_ldap_ldapurlinldapurl
description:host=} . $node->host . qq{
description:port=} . $node->port . qq{
});

# Invalid LDAP record which has ldapserviceurl parameter
append_to_file(
	$ldif_valid, qq{
version:1
dn:cn=unterminatedoptionquote,dc=example,dc=net
changetype:add
objectclass:top
objectclass:device
cn:unterminatedoptionquote
description:application_name=\'appname_from_ldap_unterminatedoptionquote
});

$ldap->ldapadd_file($ldif_valid);

my ($ldap_server, $ldap_port, $ldaps_port, $ldap_url,
	$ldaps_url, $ldap_basedn, $ldap_rootdn
) = $ldap->prop(qw(server port s_port url s_url basedn rootdn));

# don't bother to check the server's cert (though perhaps we should)
$ENV{'LDAPTLS_REQCERT'} = "never";

note "setting up PostgreSQL instance";

# Create the set of service files used in the tests.

# File that includes a valid service name, that uses a decomposed
# connection string for its contents, split on spaces.
my $srvfile_valid = "$td/pg_service_valid.conf";
append_to_file(
	$srvfile_valid, qq{
[my_srv]
ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)

[geqo_off]
ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabasegeqooff)

[ldapserviceurl_in_pg_service_test]
ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)

[pg_service_application_name]
application_name=pg_service
options=--geqo=off
});

# File defined with no contents, used as default value for
# PGSERVICEFILE, so that no lookup is attempted in the user's home
# directory.
my $srvfile_empty = "$td/pg_service_empty.conf";
append_to_file($srvfile_empty, '');

# Default service file in PGSYSCONFDIR.
my $srvfile_default = "$td/pg_service.conf";

# Missing service file.
my $srvfile_missing = "$td/pg_service_missing.conf";

# Set the fallback directory lookup of the service file to the
# temporary directory of this test.  PGSYSCONFDIR is used if the
# service file defined in PGSERVICEFILE cannot be found, or when a
# service file is found but not the service name.
local $ENV{PGSYSCONFDIR} = $td;

# Force PGSERVICEFILE to a default location, so as this test never
# tries to look at a home directory.  This value needs to remain at
# the top of this script before running any tests, and should never be
# changed.
local $ENV{PGSERVICEFILE} = "$srvfile_empty";

# Checks combinations of service name and a valid service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_valid;

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and PGSERVICEFILE',
		sql => "SELECT 'connect1_1'",
		expected_stdout => qr/connect1_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and PGSERVICEFILE',
		sql => "SELECT 'connect1_2'",
		expected_stdout => qr/connect1_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and PGSERVICEFILE',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';

	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and PGSERVICEFILE',
		sql => "SELECT 'connect1_3'",
		expected_stdout => qr/connect1_3/);

	local $ENV{PGSERVICE} = 'undefined-service';

	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and PGSERVICEFILE',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);
}

# Checks case of incorrect service file.
{
	local $ENV{PGSERVICEFILE} = $srvfile_missing;

	$dummy_node->connect_fails(
		'service=my_srv',
		'connection with correct "service" string and incorrect PGSERVICEFILE',
		expected_stderr =>
		  qr/service file ".*pg_service_missing.conf" not found/);
}

# Checks case of service file named "pg_service.conf" in PGSYSCONFDIR.
{
	# Create copy of valid file
	my $srvfile_default = "$td/pg_service.conf";
	copy($srvfile_valid, $srvfile_default);

	$dummy_node->connect_ok(
		'service=my_srv',
		'connection with correct "service" string and pg_service.conf',
		sql => "SELECT 'connect2_1'",
		expected_stdout => qr/connect2_1/);

	$dummy_node->connect_ok(
		'postgres://?service=my_srv',
		'connection with correct "service" URI and default pg_service.conf',
		sql => "SELECT 'connect2_2'",
		expected_stdout => qr/connect2_2/);

	$dummy_node->connect_fails(
		'service=undefined-service',
		'connection with incorrect "service" string and default pg_service.conf',
		expected_stderr =>
		  qr/definition of service "undefined-service" not found/);

	local $ENV{PGSERVICE} = 'my_srv';

	$dummy_node->connect_ok(
		'',
		'connection with correct PGSERVICE and default pg_service.conf',
		sql => "SELECT 'connect2_3'",
		expected_stdout => qr/connect2_3/);

	local $ENV{PGSERVICE} = 'undefined-service';

	$dummy_node->connect_fails(
		'',
		'connection with incorrect PGSERVICE and default pg_service.conf',
		expected_stdout =>
		  qr/definition of service "undefined-service" not found/);

	delete $ENV{PGSERVICE};

	unlink($srvfile_default);
}

# Test ldapserviceurl connection parameter
{
	# Create copy of valid file
	my $srvfile_default = "$td/pg_service.conf";
	copy($srvfile_valid, $srvfile_default);

	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)",
		'connection with correct "ldapserviceurl"',
		sql => "SELECT 'connect2_4', current_setting('application_name')",
		expected_stdout => qr/^connect2_4\|appname_from_ldap_mydatabase$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:1234/dc=example,dc=net?description?one?(cn=mydatabase)",
		'connection with correct "ldapserviceurl"',
		expected_stderr => qr/^psql: error: connection could not be established to ldapserviceurl: \".*\"$/);

	$dummy_node->connect_ok(
		"postgres://?ldapserviceurl=ldap%3A%2F%2Flocalhost%3A$ldap_port%2Fdc%3Dexample%2Cdc%3Dnet%3Fdescription%3Fone%3F%28cn%3Dmydatabase%29",
		'connection with correct "ldapserviceurl" in uri format',
		sql => "SELECT 'connect2_5', current_setting('application_name')",
		expected_stdout => qr/^connect2_5\|appname_from_ldap_mydatabase$/);

	local $ENV{PGLDAPSERVICEURL} = "ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)";

	$dummy_node->connect_ok(
		"",
		'connection with correct "ldapserviceurl" provided by env var',
		sql => "SELECT 'connect2_6', current_setting('application_name')",
		expected_stdout => qr/^connect2_6\|appname_from_ldap_mydatabase$/);

	$dummy_node->connect_fails(
		'ldapserviceurl=ldap://localhost:invalidport/dc=example,dc=net?description?one?(cn=mydatabase)',
		'connection fails with because valid PGLDAPSERVICEURL env var overridden by explicit parameter with bad port',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": invalid port number$/);

	delete $ENV{PGLDAPSERVICEURL};

	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase) application_name=appname_from_explicit_parameter",
		'connection with correct "ldapserviceurl" but with application_name overridden by explicit connection parameter',
		sql => "SELECT 'connect2_7', current_setting('application_name')",
		expected_stdout => qr/^connect2_7\|appname_from_explicit_parameter$/);

	local $ENV{PGAPPNAME} = "envvar_parameter";

	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)",
		'connection with correct "ldapserviceurl" but with application_name overriding envvar',
		sql => "SELECT 'connect2_8', current_setting('application_name')",
		expected_stdout => qr/^connect2_8\|appname_from_ldap_mydatabase$/);

	delete $ENV{PGAPPNAME};

	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase) service=pg_service_application_name",
		'connection with correct "ldapserviceurl" string, service file application_name overridden by ldap, geqo off set by service file',
		sql => "SELECT 'connect2_9', current_setting('application_name'), current_setting('geqo');",
		expected_stdout => qr/^connect2_9\|appname_from_ldap_mydatabase\|off$/);

	# test that geqo grabbed from service file service geqo_off via LDAP lookup
	# conflicting application names present in both LDAP services queried (ldapserviceurl and service file)
	# application name from service file lookup is ignored because it has already been set by ldapserviceurl lookup
	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase) service=geqo_off",
		'connection with 2 LDAP lookups (pg_service.conf and ldapserviceurl)',
		sql => "SELECT 'connect2_10', current_setting('application_name'), current_setting('geqo');",
		expected_stdout => qr/^connect2_10\|appname_from_ldap_mydatabase\|off$/);

	$dummy_node->connect_fails(
		'service=ldapserviceurl_in_pg_service_test',
		'connection fails with ldapserviceurl specified in pg_service.conf file',
		expected_stderr => qr/^psql: error: ldapserviceurl parameters are not supported in service file .*, line .*$/ );

	$dummy_node->connect_ok(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=ldapurlinldapurl)",
		'ldapserviceurl that points to another ldapserviceurl will be ignored',
		sql => "SELECT 'connect2_11', current_setting('application_name'), current_setting('geqo');",
		expected_stdout=> qr/^connect2_11\|appname_from_ldap_ldapurlinldapurl\|on$/);

	# Remove default pg_service.conf.
	unlink($srvfile_default);
}

# negative tests for logic inside ldapServiceLookup
{

	$dummy_node->connect_fails(
		"ldapserviceurl=http://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=mydatabase)",
		'ldapserviceurl that doesnt begin with ldap:// fails',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": scheme must be ldap:\/\/$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/",
		'ldapserviceurl that does not specify distinguished name',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": missing distinguished name$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?",
		'ldapserviceurl that does not specify attribute',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": must have exactly one attribute$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description",
		'ldapserviceurl that does not provide scope',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": must have search scope \(base\/one\/sub\)$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one",
		'ldapserviceurl that does not specify filter',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": no filter$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:invalidportnumber/dc=example,dc=net?description?one?(cn=mydatabase)", 'ldapserviceurl contains invalid port number',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": invalid port number$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description,dn?one?(cn=mydatabase)",
		'ldapserviceurl that contains 2 attributes',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": must have exactly one attribute$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?invalidscope?(cn=mydatabase)",
		'ldapserviceurl that does not provide valid scope',
		expected_stderr => qr/^psql: error: invalid LDAP URL \".*\": must have search scope \(base\/one\/sub\)$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?sub?(cn=*)",
		'ldapserviceurl that contains more than one entry',
		expected_stderr => qr/^psql: error: more than one entry found on LDAP lookup$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=doesnotexist)",
		'ldapserviceurl with no ldap entry',
		expected_stderr => qr/^psql: error: no entry found on LDAP lookup$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=noentries)",
		'ldapserviceurl that has no connection attributes',
		expected_stderr => qr/^psql: error: attribute has no values on LDAP lookup$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=missingequal)",
		'ldapservicesurl that has missing equal sign',
		expected_stderr => qr/^psql: error: missing \"\=\" after \"application_name\n\" in connection info string$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=invalidconnoptionservice)",
		'ldapserviceurl that contains nonexistent connection option',
		expected_stderr => qr/^psql: error: invalid connection option \"invalidconnoption\"$/);

	$dummy_node->connect_fails(
		"ldapserviceurl=ldap://localhost:$ldap_port/dc=example,dc=net?description?one?(cn=unterminatedoptionquote)",
		'ldapserviceurl that contains unterminated quote ',
		expected_stderr => qr/^psql: error: unterminated quoted string in connection info string$/);

}

$node->teardown_node;

done_testing();
