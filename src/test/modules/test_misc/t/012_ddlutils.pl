
# Copyright (c) 2026, PostgreSQL Global Development Group

# Tests for pg_get_database_ddl(), pg_get_tablespace_ddl(), pg_get_role_ddl(),
# and pg_get_trigger_ddl().  These are TAP tests rather than plain regression
# tests because they create databases and tablespaces, which are
# heavyweight operations that should run only once rather than being
# repeated with every invocation of the core regression suite.

use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
# Force UTC so that timestamptz values (e.g. VALID UNTIL) render the same
# way regardless of the host's local timezone.
$node->append_conf('postgresql.conf', "timezone = 'UTC'\n");
$node->start;

# Perl helper that strips locale/collation details from DDL output so
# that results are stable across platforms.
sub ddl_filter
{
	my ($text) = @_;
	$text =~ s/\s*\bLOCALE_PROVIDER\b\s*=\s*(?:'[^']*'|"[^"]*"|\S+)//gi;
	$text =~ s/\s*LC_COLLATE\s*=\s*(['"])[^'"]*\1//gi;
	$text =~ s/\s*LC_CTYPE\s*=\s*(['"])[^'"]*\1//gi;
	$text =~ s/\s*\S*LOCALE\S*\s*=?\s*(['"])[^'"]*\1//gi;
	$text =~ s/\s*\S*COLLATION\S*\s*=?\s*(['"])[^'"]*\1//gi;
	return $text;
}


########################################################################
# pg_get_role_ddl tests
########################################################################

# Basic role
$node->safe_psql('postgres', 'CREATE ROLE regress_role_ddl_test1');
my $result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_test1')});
like(
	$result,
	qr/CREATE ROLE regress_role_ddl_test1 .* NOLOGIN/,
	'basic role DDL');

# Role with multiple privileges
$node->safe_psql(
	'postgres', q{
	CREATE ROLE regress_role_ddl_test2
	  LOGIN SUPERUSER CREATEDB CREATEROLE
	  CONNECTION LIMIT 5
	  VALID UNTIL '2030-12-31 23:59:59+00'});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_test2')});
like($result, qr/SUPERUSER/, 'role with SUPERUSER');
like($result, qr/CREATEDB/, 'role with CREATEDB');
like($result, qr/CONNECTION LIMIT 5/, 'role with CONNECTION LIMIT');
like($result, qr/VALID UNTIL '2030-12-31/, 'role with VALID UNTIL');

# Role with configuration parameters
$node->safe_psql(
	'postgres', q{
	ALTER ROLE regress_role_ddl_test1 SET work_mem TO '256MB';
	ALTER ROLE regress_role_ddl_test1 SET search_path TO myschema, public});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_test1')});
like($result, qr/SET work_mem TO '256MB'/, 'role with work_mem setting');
like($result, qr/SET search_path TO/, 'role with search_path setting');

# Role with database-specific configuration (needs a real database)
$node->safe_psql(
	'postgres', q{
	CREATE DATABASE regression_ddlutils_test
	  TEMPLATE template0 ENCODING 'UTF8' LC_COLLATE 'C' LC_CTYPE 'C';
	ALTER ROLE regress_role_ddl_test2
	  IN DATABASE regression_ddlutils_test SET work_mem TO '128MB'});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_test2')});
like(
	$result,
	qr/IN DATABASE regression_ddlutils_test SET work_mem TO '128MB'/,
	'role with database-specific setting');

# Role with special characters (requires quoting)
$node->safe_psql('postgres', q{CREATE ROLE "regress_role-with-dash"});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role-with-dash')});
like($result, qr/"regress_role-with-dash"/, 'role name requiring quoting');

# Pretty-printed output
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_test2', pretty => true)}
);
like($result, qr/\n\s+SUPERUSER/, 'role pretty-print indents attributes');

# Role with memberships
$node->safe_psql(
	'postgres', q{
	CREATE ROLE regress_role_ddl_grantor CREATEROLE;
	CREATE ROLE regress_role_ddl_group1;
	CREATE ROLE regress_role_ddl_group2;
	CREATE ROLE regress_role_ddl_member;
	GRANT regress_role_ddl_group1 TO regress_role_ddl_grantor WITH ADMIN TRUE;
	GRANT regress_role_ddl_group2 TO regress_role_ddl_grantor WITH ADMIN TRUE;
	SET ROLE regress_role_ddl_grantor;
	GRANT regress_role_ddl_group1 TO regress_role_ddl_member
	  WITH INHERIT TRUE, SET FALSE;
	GRANT regress_role_ddl_group2 TO regress_role_ddl_member
	  WITH ADMIN TRUE;
	RESET ROLE});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_member')});
like(
	$result,
	qr/GRANT regress_role_ddl_group1 TO regress_role_ddl_member/,
	'role with memberships includes GRANT');
like($result, qr/SET FALSE/, 'membership includes SET FALSE');
like($result, qr/ADMIN TRUE/, 'membership includes ADMIN TRUE');

# Memberships suppressed
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_role_ddl('regress_role_ddl_member', memberships => false)}
);
unlike($result, qr/GRANT/, 'memberships suppressed');

# Non-existent role (should error)
my ($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT * FROM pg_get_role_ddl(9999999::oid)});
isnt($ret, 0, 'non-existent role errors');
like($stderr, qr/does not exist/, 'non-existent role error message');

# NULL input (should return no rows)
$result =
  $node->safe_psql('postgres', q{SELECT count(*) FROM pg_get_role_ddl(NULL)});
is($result, '0', 'NULL role returns no rows');

# Permission check: revoke SELECT on pg_authid
$node->safe_psql(
	'postgres', q{
	CREATE ROLE regress_role_ddl_noaccess;
	REVOKE SELECT ON pg_authid FROM PUBLIC});
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SET ROLE regress_role_ddl_noaccess;
	  SELECT * FROM pg_get_role_ddl('regress_role_ddl_test1')});
isnt($ret, 0, 'role DDL denied without pg_authid access');
$node->safe_psql(
	'postgres', q{
	GRANT SELECT ON pg_authid TO PUBLIC});


########################################################################
# pg_get_database_ddl tests
########################################################################

# Set up: the test database was already created above for role tests.
$node->safe_psql(
	'postgres', q{
	ALTER DATABASE regression_ddlutils_test OWNER TO regress_role_ddl_test2;
	ALTER DATABASE regression_ddlutils_test CONNECTION LIMIT 123;
	ALTER DATABASE regression_ddlutils_test SET random_page_cost = 2.0;
	ALTER ROLE regress_role_ddl_test2
	  IN DATABASE regression_ddlutils_test SET random_page_cost = 1.1});

# Non-existent database
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SELECT * FROM pg_get_database_ddl('regression_no_such_db')});
isnt($ret, 0, 'non-existent database errors');

# NULL input
$result = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_get_database_ddl(NULL)});
is($result, '0', 'NULL database returns no rows');

# Invalid option (bad boolean cast)
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SELECT * FROM pg_get_database_ddl('regression_ddlutils_test', owner => 'invalid')}
);
isnt($ret, 0, 'invalid boolean option errors');
like(
	$stderr,
	qr/invalid input syntax for type boolean/,
	'invalid option error message');

# Duplicate named argument
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SELECT * FROM pg_get_database_ddl('regression_ddlutils_test',
	  owner => false, owner => true)});
isnt($ret, 0, 'duplicate option errors');

# Basic output (without locale details)
$result = ddl_filter(
	$node->safe_psql(
		'postgres',
		q{SELECT pg_get_database_ddl
	  FROM pg_get_database_ddl('regression_ddlutils_test')}));
like(
	$result,
	qr/CREATE DATABASE regression_ddlutils_test/,
	'database DDL includes CREATE');
like($result, qr/TEMPLATE = template0/, 'database DDL includes TEMPLATE');
like($result, qr/ENCODING = 'UTF8'/, 'database DDL includes ENCODING');
like(
	$result,
	qr/OWNER TO regress_role_ddl_test2/,
	'database DDL includes OWNER');
like($result, qr/CONNECTION LIMIT = 123/, 'database DDL includes CONNLIMIT');
like(
	$result,
	qr/SET random_page_cost TO '2.0'/,
	'database DDL includes GUC setting');

# Pretty-printed output
$result = ddl_filter(
	$node->safe_psql(
		'postgres',
		q{SELECT pg_get_database_ddl
	  FROM pg_get_database_ddl('regression_ddlutils_test',
	    pretty => true, tablespace => false)}));
like($result, qr/\n\s+WITH TEMPLATE/, 'database DDL pretty-prints WITH');

# Owner suppressed
$result = ddl_filter(
	$node->safe_psql(
		'postgres',
		q{SELECT pg_get_database_ddl
	  FROM pg_get_database_ddl('regression_ddlutils_test', owner => false)}));
unlike($result, qr/OWNER/, 'database DDL owner suppressed');

# Permission check
$node->safe_psql(
	'postgres', q{
	REVOKE CONNECT ON DATABASE regression_ddlutils_test FROM PUBLIC});
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SET ROLE regress_role_ddl_noaccess;
	  SELECT * FROM pg_get_database_ddl('regression_ddlutils_test')});
isnt($ret, 0, 'database DDL denied without CONNECT');
$node->safe_psql(
	'postgres', q{
	GRANT CONNECT ON DATABASE regression_ddlutils_test TO PUBLIC});


########################################################################
# pg_get_tablespace_ddl tests
########################################################################

# Non-existent tablespace by name
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SELECT * FROM pg_get_tablespace_ddl('regress_nonexistent_tblsp')});
isnt($ret, 0, 'non-existent tablespace errors');

# Non-existent tablespace by OID
($ret, $stdout, $stderr) =
  $node->psql('postgres', q{SELECT * FROM pg_get_tablespace_ddl(0::oid)});
isnt($ret, 0, 'non-existent tablespace OID errors');

# NULL input (name and OID variants)
$result = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_get_tablespace_ddl(NULL::name)});
is($result, '0', 'NULL tablespace name returns no rows');
$result = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_get_tablespace_ddl(NULL::oid)});
is($result, '0', 'NULL tablespace OID returns no rows');

# Tablespace name requiring quoting
$node->safe_psql(
	'postgres', q{
	SET allow_in_place_tablespaces = true;
	CREATE TABLESPACE "regress_ tblsp" OWNER regress_role_ddl_test1
	  LOCATION ''});
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_tablespace_ddl('regress_ tblsp')});
like($result, qr/"regress_ tblsp"/, 'tablespace name is quoted');

# Rename and add options; reuse this tablespace for the remaining tests
$node->safe_psql(
	'postgres', q{
	ALTER TABLESPACE "regress_ tblsp" RENAME TO regress_allopt_tblsp;
	ALTER TABLESPACE regress_allopt_tblsp
	  SET (seq_page_cost = '1.5', random_page_cost = '1.1234567890',
	       effective_io_concurrency = '17', maintenance_io_concurrency = '18')});

# Tablespace with multiple options
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp')});
like(
	$result,
	qr/CREATE TABLESPACE regress_allopt_tblsp/,
	'tablespace DDL includes CREATE');
like(
	$result,
	qr/OWNER regress_role_ddl_test1/,
	'tablespace DDL includes OWNER');
like($result, qr/seq_page_cost='1.5'/, 'tablespace DDL includes options');

# Pretty-printed output
$result = $node->safe_psql(
	'postgres',
	q{SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp',
	  pretty => true)});
like($result, qr/\n\s+OWNER/, 'tablespace DDL pretty-prints OWNER');

# Owner suppressed
$result = $node->safe_psql(
	'postgres',
	q{SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp',
	  owner => false)});
unlike($result, qr/OWNER/, 'tablespace DDL owner suppressed');

# Lookup by OID
$result = $node->safe_psql(
	'postgres', q{
	SELECT pg_get_tablespace_ddl
	FROM pg_get_tablespace_ddl(
	  (SELECT oid FROM pg_tablespace
	   WHERE spcname = 'regress_allopt_tblsp'))});
like(
	$result,
	qr/CREATE TABLESPACE regress_allopt_tblsp/,
	'tablespace DDL by OID');

# Permission check
$node->safe_psql('postgres', q{REVOKE SELECT ON pg_tablespace FROM PUBLIC});
($ret, $stdout, $stderr) = $node->psql(
	'postgres',
	q{SET ROLE regress_role_ddl_noaccess;
	  SELECT * FROM pg_get_tablespace_ddl('regress_allopt_tblsp')});
isnt($ret, 0, 'tablespace DDL denied without pg_tablespace access');
$node->safe_psql(
	'postgres', q{
	GRANT SELECT ON pg_tablespace TO PUBLIC});

########################################################################
# pg_get_trigger_ddl tests
########################################################################

$node->safe_psql(
	'postgres', q{
	CREATE FUNCTION regress_trg_ddl_func() RETURNS trigger
	  LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;
	CREATE TABLE regress_trg_ddl_tab (id int, note text, "before" int);
	CREATE TRIGGER regress_trg_ddl_basic
	  BEFORE INSERT ON regress_trg_ddl_tab
	  FOR EACH ROW EXECUTE FUNCTION regress_trg_ddl_func();
	CREATE TRIGGER regress_trg_ddl_when
	  BEFORE UPDATE OF id ON regress_trg_ddl_tab
	  FOR EACH ROW WHEN (OLD.id IS DISTINCT FROM NEW.id)
	  EXECUTE FUNCTION regress_trg_ddl_func()});

my $trig_oid = $node->safe_psql(
	'postgres', q{
	SELECT t.oid FROM pg_trigger t
	  JOIN pg_class c ON c.oid = t.tgrelid
	  WHERE t.tgname = 'regress_trg_ddl_basic'
	    AND c.relname = 'regress_trg_ddl_tab'});

# Explicit pretty => false gives the canonical pg_get_triggerdef form
# plus a trailing semicolon.
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_basic', false)});
my $plain = $node->safe_psql('postgres',
	qq{SELECT pg_get_triggerdef($trig_oid, false) || ';'});
is($result, $plain, 'trigger DDL matches pg_get_triggerdef plus semicolon');
like($result, qr/ ON public\.regress_trg_ddl_tab /,
	'trigger DDL schema-qualifies the table');

# Explicit regclass cast works too
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab'::regclass, 'regress_trg_ddl_basic', false)});
is($result, $plain, 'trigger DDL by regclass matches');

# Omitting pretty defaults to false
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_basic')});
is($result, $plain, 'omitting pretty defaults to false');

# Non-existent trigger name errors
($ret, $stdout, $stderr) = $node->psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_missing')});
isnt($ret, 0, 'non-existent trigger name errors');
like($stderr, qr/does not exist/, 'non-existent trigger error message');

# NULL inputs return no rows
$result = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_get_trigger_ddl(NULL::regclass, 'regress_trg_ddl_basic')});
is($result, '0', 'NULL relation returns no rows');
$result = $node->safe_psql('postgres',
	q{SELECT count(*) FROM pg_get_trigger_ddl('regress_trg_ddl_tab', NULL::name)});
is($result, '0', 'NULL trigger name returns no rows');


# --- pretty layout ---

$node->safe_psql(
	'postgres', q{
	CREATE TABLE regress_trg_ddl_ref (id int PRIMARY KEY);
	CREATE VIEW regress_trg_ddl_view AS SELECT * FROM regress_trg_ddl_tab;
	CREATE TRIGGER regress_trg_ddl_trans
	  AFTER INSERT ON regress_trg_ddl_tab
	  REFERENCING NEW TABLE AS newtab
	  FOR EACH STATEMENT EXECUTE FUNCTION regress_trg_ddl_func();
	CREATE CONSTRAINT TRIGGER regress_trg_ddl_con
	  AFTER INSERT ON regress_trg_ddl_tab
	  FROM regress_trg_ddl_ref
	  DEFERRABLE INITIALLY DEFERRED
	  FOR EACH ROW EXECUTE FUNCTION regress_trg_ddl_func();
	CREATE TRIGGER regress_trg_ddl_instead
	  INSTEAD OF INSERT ON regress_trg_ddl_view
	  FOR EACH ROW EXECUTE FUNCTION regress_trg_ddl_func();
	CREATE TRIGGER "when"
	  BEFORE UPDATE OF "before" ON regress_trg_ddl_tab
	  FOR EACH ROW WHEN (NEW.note <> ' EXECUTE FUNCTION ')
	  EXECUTE FUNCTION regress_trg_ddl_func('FOR EACH ')});

# Exact pretty layout for a simple trigger
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_basic', true)});
is( $result,
	qq{CREATE TRIGGER regress_trg_ddl_basic
    BEFORE INSERT ON public.regress_trg_ddl_tab
    FOR EACH ROW
    EXECUTE FUNCTION regress_trg_ddl_func();},
	'pretty trigger DDL layout for simple trigger');

# 'pretty' accepts an explicit false
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_basic', false)});
is($result, $plain, 'pretty => false gives the single-line form');

# Invariant: collapsing pretty whitespace reproduces the canonical form.
# This holds for every trigger, so check it across all of them.
foreach my $pair (
	[ 'regress_trg_ddl_tab', 'regress_trg_ddl_basic' ],
	[ 'regress_trg_ddl_tab', 'regress_trg_ddl_when' ],
	[ 'regress_trg_ddl_tab', 'regress_trg_ddl_trans' ],
	[ 'regress_trg_ddl_tab', 'regress_trg_ddl_con' ],
	[ 'regress_trg_ddl_view', 'regress_trg_ddl_instead' ],
	[ 'regress_trg_ddl_tab', 'when' ])
{
	my ($rel, $tg) = @$pair;
	my $pretty = $node->safe_psql('postgres',
		qq{SELECT * FROM pg_get_trigger_ddl('$rel', '$tg', true)});
	my $single = $node->safe_psql('postgres',
		qq{SELECT * FROM pg_get_trigger_ddl('$rel', '$tg', false)});
	(my $collapsed = $pretty) =~ s/\n    / /g;
	is($collapsed, $single, "pretty output for $tg collapses to canonical form");
}

# WHEN clause: the IS DISTINCT FROM inside parens must not be split
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_when', true)});
like($result, qr/\n    WHEN \(/, 'pretty WHEN clause on its own line');
like($result, qr/IS DISTINCT FROM new\.id/,
	'FROM inside WHEN expression not split');

# Transition tables and constraint trigger clauses each get a line
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_trans', true)});
like($result, qr/\n    REFERENCING NEW TABLE AS newtab/,
	'pretty REFERENCING clause on its own line');
like($result, qr/\n    FOR EACH STATEMENT/,
	'pretty FOR EACH STATEMENT on its own line');
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'regress_trg_ddl_con', true)});
like($result, qr/\n    FROM .*regress_trg_ddl_ref/,
	'pretty FROM clause on its own line');
like($result, qr/\n    DEFERRABLE INITIALLY DEFERRED/,
	'pretty DEFERRABLE clause on its own line');

# INSTEAD OF triggers (on a view)
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_view', 'regress_trg_ddl_instead', true)});
like($result, qr/\n    INSTEAD OF INSERT ON public\.regress_trg_ddl_view/,
	'pretty INSTEAD OF clause on its own line');

# Adversarial: keyword-named identifiers are quoted and must not attract
# breaks; clause keywords inside string literals must survive intact.
$result = $node->safe_psql('postgres',
	q{SELECT * FROM pg_get_trigger_ddl('regress_trg_ddl_tab', 'when', true)});
like($result, qr/^CREATE TRIGGER "when"$/m,
	'quoted keyword trigger name stays on the first line');
like($result, qr/\n    BEFORE UPDATE OF before ON public\.regress_trg_ddl_tab/,
	'lowercase unreserved-keyword column does not attract a line break');
like($result, qr/ EXECUTE FUNCTION '/,
	'marker text inside WHEN string literal not split');
like($result, qr/\('FOR EACH '\)/,
	'marker text inside trigger argument literal not split');

$node->stop;

done_testing();
