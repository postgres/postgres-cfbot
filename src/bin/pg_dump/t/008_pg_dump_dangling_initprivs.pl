# Copyright (c) 2024-2026, PostgreSQL Global Development Group
#
# Tests that pg_dump silently skips pg_init_privs entries that reference
# roles no longer present in pg_authid, rather than emitting invalid GRANT
# statements with numeric OIDs as role names.

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE regress_dangling');

# --- Setup ---
# Simulate dangling pg_init_privs entries by inserting grants for a role
# and then deleting the role directly from pg_authid (bypassing pg_shdepend).
$node->safe_psql(
	'regress_dangling',
	q{
SET allow_system_table_mods = true;

-- Roles for testing
CREATE ROLE ghost_grantee;
CREATE ROLE ghost_grantor;
CREATE ROLE "007";
CREATE ROLE regress_col_grantee;
CREATE ROLE ghost_col_revoke;

-- Case 1: dangling grantee (function)
CREATE FUNCTION public.test_func_grantee() RETURNS int LANGUAGE sql AS 'SELECT 1';
REVOKE ALL ON FUNCTION public.test_func_grantee() FROM PUBLIC;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT p.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
       0, 'e',
       ARRAY[('ghost_grantee=X/' || quote_ident(current_user))::aclitem]
FROM pg_proc p
WHERE p.proname = 'test_func_grantee'
  AND p.pronamespace = 'public'::regnamespace;

-- Case 2: dangling grantor (function)
CREATE FUNCTION public.test_func_grantor() RETURNS int LANGUAGE sql AS 'SELECT 2';
REVOKE ALL ON FUNCTION public.test_func_grantor() FROM PUBLIC;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT p.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
       0, 'e',
       ARRAY[(quote_ident(current_user) || '=X/ghost_grantor')::aclitem]
FROM pg_proc p
WHERE p.proname = 'test_func_grantor'
  AND p.pronamespace = 'public'::regnamespace;

-- Case 3: dangling column-level grantee (table)
CREATE TABLE public.test_tbl (id int, secret text);
GRANT SELECT (secret) ON public.test_tbl TO regress_col_grantee;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT c.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_class'),
       2, 'e',
       ARRAY[('ghost_grantee=r/' || quote_ident(current_user))::aclitem]
FROM pg_class c
WHERE c.relname = 'test_tbl'
  AND c.relnamespace = 'public'::regnamespace;

-- Case 4: a column that was *never* explicitly granted anything (its
-- actual attacl stays NULL) but carries an all-dangling pg_init_privs
-- entry.  The column-ACL query picks up a column whenever it has *any*
-- pg_init_privs row, regardless of whether attacl is NULL, so this is a
-- genuinely reachable "spurious REVOKE" path -- unlike a plain function
-- with NULL proacl, which pg_dump never even considers for ACL output
-- (DUMP_COMPONENT_ACL is only set there when the actual ACL is non-NULL).
CREATE TABLE public.test_tbl_nullacl (id int, secret text);
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT c.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_class'),
       2, 'e',
       ARRAY[('ghost_col_revoke=r/' || quote_ident(current_user))::aclitem]
FROM pg_class c
WHERE c.relname = 'test_tbl_nullacl'
  AND c.relnamespace = 'public'::regnamespace;

-- Case 5: valid all-digit role "007" with a grant (must NOT be filtered)
CREATE FUNCTION public.test_func_007() RETURNS int LANGUAGE sql AS 'SELECT 7';
GRANT EXECUTE ON FUNCTION public.test_func_007() TO "007";

-- Case 6: PUBLIC grant whose grantor is dangling (must be filtered)
CREATE FUNCTION public.test_func_public_ghost() RETURNS int LANGUAGE sql AS 'SELECT 6';
REVOKE ALL ON FUNCTION public.test_func_public_ghost() FROM PUBLIC;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT p.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
       0, 'e',
       ARRAY['=X/ghost_grantor'::aclitem]
FROM pg_proc p
WHERE p.proname = 'test_func_public_ghost'
  AND p.pronamespace = 'public'::regnamespace;

-- Case 7: PUBLIC grant with a valid grantor (must NOT be filtered)
CREATE FUNCTION public.test_func_public_ok() RETURNS int LANGUAGE sql AS 'SELECT 7';
REVOKE ALL ON FUNCTION public.test_func_public_ok() FROM PUBLIC;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT p.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
       0, 'e',
       ARRAY[('=X/' || quote_ident(current_user))::aclitem]
FROM pg_proc p
WHERE p.proname = 'test_func_public_ok'
  AND p.pronamespace = 'public'::regnamespace;

-- Case 8: dangling grantee on an aggregate. Aggregates are stored in
-- pg_proc like plain functions and share the same ACL-diff machinery, but
-- they are fetched by a separate query (getAggregates(), not getFuncs()),
-- so this exercises a genuinely different code path than case 1.
CREATE AGGREGATE public.test_agg_grantee (int4) (SFUNC = int4pl, STYPE = int4, INITCOND = '0');
REVOKE ALL ON FUNCTION public.test_agg_grantee(int4) FROM PUBLIC;
INSERT INTO pg_init_privs (objoid, classoid, objsubid, privtype, initprivs)
SELECT p.oid,
       (SELECT oid FROM pg_class WHERE relname = 'pg_proc'),
       0, 'e',
       ARRAY[('ghost_grantee=X/' || quote_ident(current_user))::aclitem]
FROM pg_proc p
WHERE p.proname = 'test_agg_grantee'
  AND p.pronamespace = 'public'::regnamespace;

-- Now delete the ghost roles to create dangling OIDs
DELETE FROM pg_authid WHERE rolname = 'ghost_grantee';
DELETE FROM pg_authid WHERE rolname = 'ghost_grantor';
DELETE FROM pg_authid WHERE rolname = 'ghost_col_revoke';


});

my $tempdir   = PostgreSQL::Test::Utils::tempdir;
my $dump_file = "$tempdir/dangling.sql";

# pg_dump must succeed even with dangling pg_init_privs entries.
command_ok(
	[
		'pg_dump',
		'--port'        => $node->port,
		'--schema-only',
		'-f'            => $dump_file,
		'regress_dangling',
	],
	'pg_dump succeeds with dangling pg_init_privs entries');

my $dump = slurp_file($dump_file);

# --- Case 1: dangling grantee ---
like($dump, qr/CREATE FUNCTION public\.test_func_grantee/,
	'case 1: function is present in dump');
unlike($dump, qr/GRANT\b.*\btest_func_grantee/,
	'case 1: no GRANT for function with dangling grantee');

# --- Case 2: dangling grantor ---
like($dump, qr/CREATE FUNCTION public\.test_func_grantor/,
	'case 2: function is present in dump');
unlike($dump, qr/GRANT\b.*\btest_func_grantor/,
	'case 2: no GRANT for function with dangling grantor');

# --- Case 3: column-level dangling ---
like($dump, qr/CREATE TABLE public\.test_tbl/,
	'case 3: table is present in dump');
unlike($dump, qr/REVOKE\b.*\btest_tbl\b/,
	'case 3: no column-level REVOKE naming a dangling OID');

# --- Case 4: never-granted column with all-dangling initprivs ---
like($dump, qr/CREATE TABLE public\.test_tbl_nullacl/,
	'case 4: table is present in dump');
unlike($dump, qr/REVOKE\b.*\btest_tbl_nullacl\b/,
	'case 4: no spurious REVOKE for never-granted column with all-dangling initprivs');

# --- Case 5: valid all-digit role "007" ---
like($dump, qr/CREATE FUNCTION public\.test_func_007/,
	'case 5: function is present in dump');
like($dump, qr/GRANT\b.*\btest_func_007\b.*TO\s+"007"/,
	'case 5: GRANT to valid all-digit role "007" is preserved');

# --- General: no numeric OID as role name (other than the valid "007") ---
# Match any GRANT/REVOKE naming a role as "digits", except the valid "007".
unlike($dump, qr/^(?:GRANT|REVOKE)\b.*\b(?:TO|FROM)\s+"(?!007")[0-9]+"/m,
	'no GRANT/REVOKE with bare numeric OID as role name (other than valid "007")');

# --- Case 6: PUBLIC entry with dangling grantor is filtered ---
unlike($dump, qr/GRANT\b.*\btest_func_public_ghost/,
	'case 6: no GRANT for PUBLIC entry with dangling grantor');

# --- Case 7: PUBLIC entry with valid grantor survives the filter ---
like($dump, qr/GRANT\b.*\btest_func_public_ok/,
	'case 7: PUBLIC entry with valid grantor is preserved');

# --- Case 8: dangling grantee on an aggregate (getAggregates() path) ---
like($dump, qr/CREATE AGGREGATE public\.test_agg_grantee/,
	'case 8: aggregate is present in dump');
unlike($dump, qr/GRANT\b.*\btest_agg_grantee/,
	'case 8: no GRANT for aggregate with dangling grantee');

# --- Case 9: --binary-upgrade has its own ACL-reconstruction code path in
# dumpACL() (the binary_upgrade_set_record_init_privs() preamble) that a
# plain dump never exercises, so it needs its own check against the same
# dangling data. ---
my $dump_file_bu = "$tempdir/dangling_binary_upgrade.sql";
command_ok(
	[
		'pg_dump',
		'--port'          => $node->port,
		'--schema-only',
		'--binary-upgrade',
		'-f'              => $dump_file_bu,
		'regress_dangling',
	],
	'pg_dump --binary-upgrade succeeds with dangling pg_init_privs entries');

my $dump_bu = slurp_file($dump_file_bu);

unlike(
	$dump_bu,
	qr/^(?:GRANT|REVOKE)\b.*\b(?:TO|FROM)\s+"(?!007")[0-9]+"/m,
	'case 9: no GRANT/REVOKE with bare numeric OID under --binary-upgrade');

# A dangling *grantor* never appears as a numeric role name in GRANT or
# REVOKE: it appears in the SET SESSION AUTHORIZATION that dumpACL() wraps
# the initial-privileges block in, which is the command that failed in the
# original report.  The check above cannot see that, so check it separately.
unlike($dump_bu, qr/^SET SESSION AUTHORIZATION "(?!007")[0-9]+"/m,
	'case 9: no SET SESSION AUTHORIZATION naming a bare numeric OID');

done_testing();
