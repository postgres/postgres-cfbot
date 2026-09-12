CREATE SCHEMA has$dollar;

-- test some errors
CREATE EXTENSION test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext1;
CREATE EXTENSION test_ext1 SCHEMA test_ext;
CREATE EXTENSION test_ext1 SCHEMA has$dollar;

-- finally success
CREATE EXTENSION test_ext1 SCHEMA has$dollar CASCADE;

SELECT extname, nspname, extversion, extrelocatable FROM pg_extension e, pg_namespace n WHERE extname LIKE 'test_ext%' AND e.extnamespace = n.oid ORDER BY 1;

CREATE EXTENSION test_ext_cyclic1 CASCADE;

DROP SCHEMA has$dollar CASCADE;
CREATE SCHEMA has$dollar;

CREATE EXTENSION test_ext6;
DROP EXTENSION test_ext6;
CREATE EXTENSION test_ext6;

-- test dropping of member tables that own extensions:
-- this table will be absorbed into test_ext7
create table old_table1 (col1 serial primary key);
create extension test_ext7;
\dx+ test_ext7
alter extension test_ext7 update to '2.0';
\dx+ test_ext7

-- test reporting of errors in extension scripts
alter extension test_ext7 update to '2.1bad';
alter extension test_ext7 update to '2.2bad';

-- test handling of temp objects created by extensions
create extension test_ext8;

-- \dx+ would expose a variable pg_temp_nn schema name, so we can't use it here
select regexp_replace(pg_describe_object(classid, objid, objsubid),
                      'pg_temp_\d+', 'pg_temp', 'g') as "Object description"
from pg_depend
where refclassid = 'pg_extension'::regclass and deptype = 'e' and
  refobjid = (select oid from pg_extension where extname = 'test_ext8')
order by 1;

-- Should be possible to drop and recreate this extension
drop extension test_ext8;
create extension test_ext8;

select regexp_replace(pg_describe_object(classid, objid, objsubid),
                      'pg_temp_\d+', 'pg_temp', 'g') as "Object description"
from pg_depend
where refclassid = 'pg_extension'::regclass and deptype = 'e' and
  refobjid = (select oid from pg_extension where extname = 'test_ext8')
order by 1;

-- here we want to start a new session and wait till old one is gone
select pg_backend_pid() as oldpid \gset
\c -
do 'declare c int = 0;
begin
  while (select count(*) from pg_stat_activity where pid = '
    :'oldpid'
  ') > 0 loop c := c + 1; perform pg_stat_clear_snapshot(); end loop;
  raise log ''test_extensions looped % times'', c;
end';

-- extension should now contain no temp objects
\dx+ test_ext8

-- dropping it should still work
drop extension test_ext8;

-- check handling of types as extension members
create extension test_ext9;
\dx+ test_ext9
alter extension test_ext9 drop type varbitrange;
\dx+ test_ext9
alter extension test_ext9 add type varbitrange;
\dx+ test_ext9
alter extension test_ext9 drop table sometable;
\dx+ test_ext9
alter extension test_ext9 add table sometable;
\dx+ test_ext9
drop extension test_ext9;

-- Test creation of extension in temporary schema with two-phase commit,
-- which should not work.  This function wrapper is useful for portability.

-- Avoid noise caused by CONTEXT and NOTICE messages including the temporary
-- schema name.
\set SHOW_CONTEXT never
SET client_min_messages TO 'warning';
-- First enforce presence of temporary schema.
CREATE TEMP TABLE test_ext4_tab ();
CREATE OR REPLACE FUNCTION create_extension_with_temp_schema()
  RETURNS VOID AS $$
  DECLARE
    tmpschema text;
    query text;
  BEGIN
    SELECT INTO tmpschema pg_my_temp_schema()::regnamespace;
    query := 'CREATE EXTENSION test_ext4 SCHEMA ' || tmpschema || ' CASCADE;';
    RAISE NOTICE 'query %', query;
    EXECUTE query;
  END; $$ LANGUAGE plpgsql;
BEGIN;
SELECT create_extension_with_temp_schema();
PREPARE TRANSACTION 'twophase_extension';
-- Clean up
DROP TABLE test_ext4_tab;
DROP FUNCTION create_extension_with_temp_schema();
RESET client_min_messages;
\unset SHOW_CONTEXT

-- Test case of an event trigger run in an extension upgrade script.
-- See: https://postgr.es/m/20200902193715.6e0269d4@firost
CREATE EXTENSION test_ext_evttrig;
ALTER EXTENSION test_ext_evttrig UPDATE TO '2.0';
DROP EXTENSION test_ext_evttrig;

-- It's generally bad style to use CREATE OR REPLACE unnecessarily.
-- Test what happens if an extension does it anyway.
-- Replacing a shell type or operator is sort of like CREATE OR REPLACE;
-- check that too.

CREATE FUNCTION ext_cor_func() RETURNS text
  AS $$ SELECT 'ext_cor_func: original'::text $$ LANGUAGE sql;

CREATE EXTENSION test_ext_cor;  -- fail

SELECT ext_cor_func();

DROP FUNCTION ext_cor_func();

CREATE VIEW ext_cor_view AS
  SELECT 'ext_cor_view: original'::text AS col;

CREATE EXTENSION test_ext_cor;  -- fail

SELECT ext_cor_func();

SELECT * FROM ext_cor_view;

DROP VIEW ext_cor_view;

CREATE TYPE test_ext_type;

CREATE EXTENSION test_ext_cor;  -- fail

DROP TYPE test_ext_type;

-- this makes a shell "point <<@@ polygon" operator too
CREATE OPERATOR @@>> ( PROCEDURE = poly_contain_pt,
  LEFTARG = polygon, RIGHTARG = point,
  COMMUTATOR = <<@@ );

CREATE EXTENSION test_ext_cor;  -- fail

DROP OPERATOR <<@@ (point, polygon);

CREATE EXTENSION test_ext_cor;  -- now it should work

SELECT ext_cor_func();

SELECT * FROM ext_cor_view;

SELECT 'x'::test_ext_type;

SELECT point(0,0) <<@@ polygon(circle(point(0,0),1));

\dx+ test_ext_cor

--
-- CREATE IF NOT EXISTS is an entirely unsound thing for an extension
-- to be doing, but let's at least plug the major security hole in it.
--

CREATE COLLATION ext_cine_coll
  ( LC_COLLATE = "C", LC_CTYPE = "C" );

CREATE EXTENSION test_ext_cine;  -- fail

DROP COLLATION ext_cine_coll;

CREATE MATERIALIZED VIEW ext_cine_mv AS SELECT 11 AS f1;

CREATE EXTENSION test_ext_cine;  -- fail

DROP MATERIALIZED VIEW ext_cine_mv;

CREATE FOREIGN DATA WRAPPER dummy;

CREATE SERVER ext_cine_srv FOREIGN DATA WRAPPER dummy;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SERVER ext_cine_srv;

CREATE SCHEMA ext_cine_schema;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SCHEMA ext_cine_schema;

CREATE SEQUENCE ext_cine_seq;

CREATE EXTENSION test_ext_cine;  -- fail

DROP SEQUENCE ext_cine_seq;

CREATE TABLE ext_cine_tab1 (x int);

CREATE EXTENSION test_ext_cine;  -- fail

DROP TABLE ext_cine_tab1;

CREATE TABLE ext_cine_tab2 AS SELECT 42 AS y;

CREATE EXTENSION test_ext_cine;  -- fail

DROP TABLE ext_cine_tab2;

CREATE EXTENSION test_ext_cine;

\dx+ test_ext_cine

ALTER EXTENSION test_ext_cine UPDATE TO '1.1';

\dx+ test_ext_cine

--
-- Test @extschema@ syntax.
--
CREATE SCHEMA "has space";
CREATE EXTENSION test_ext_extschema SCHEMA has$dollar;
CREATE EXTENSION test_ext_extschema SCHEMA "has space";

--
-- Test basic SET SCHEMA handling.
--
CREATE SCHEMA s1;
CREATE SCHEMA s2;
CREATE EXTENSION test_ext_set_schema SCHEMA s1;
ALTER EXTENSION test_ext_set_schema SET SCHEMA s2;
\dx+ test_ext_set_schema
\sf s2.ess_func(int)

--
-- Test extension with objects outside the extension's schema.
--
CREATE SCHEMA test_func_dep1;
CREATE SCHEMA test_func_dep2;
CREATE SCHEMA test_func_dep3;
CREATE EXTENSION test_ext_req_schema1 SCHEMA test_func_dep1;
ALTER FUNCTION test_func_dep1.dep_req1() SET SCHEMA test_func_dep2;
SELECT pg_describe_object(classid, objid, objsubid) as obj,
       pg_describe_object(refclassid, refobjid, refobjsubid) as objref,
       deptype
  FROM pg_depend
  WHERE classid = 'pg_extension'::regclass AND
        objid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_req_schema1')
  ORDER BY 1, 2;
-- fails, as function dep_req1 is not in the same schema as the extension.
ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_func_dep3;
-- Move back the function, and the extension can be moved.
ALTER FUNCTION test_func_dep2.dep_req1() SET SCHEMA test_func_dep1;
ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_func_dep3;
SELECT pg_describe_object(classid, objid, objsubid) as obj,
       pg_describe_object(refclassid, refobjid, refobjsubid) as objref,
       deptype
  FROM pg_depend
  WHERE classid = 'pg_extension'::regclass AND
        objid = (SELECT oid FROM pg_extension WHERE extname = 'test_ext_req_schema1')
  ORDER BY 1, 2;
DROP EXTENSION test_ext_req_schema1 CASCADE;
DROP SCHEMA test_func_dep1;
DROP SCHEMA test_func_dep2;
DROP SCHEMA test_func_dep3;

--
-- Test @extschema:extname@ syntax and no_relocate option
--
CREATE EXTENSION test_ext_req_schema1 SCHEMA has$dollar;
CREATE EXTENSION test_ext_req_schema3 CASCADE;
DROP EXTENSION test_ext_req_schema1;
CREATE SCHEMA test_s_dep;
CREATE EXTENSION test_ext_req_schema1 SCHEMA test_s_dep;
CREATE EXTENSION test_ext_req_schema3 CASCADE;
SELECT test_s_dep.dep_req1();
SELECT dep_req2();
SELECT dep_req3();
SELECT dep_req3b();
CREATE SCHEMA test_s_dep2;
ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_s_dep2;  -- fails
ALTER EXTENSION test_ext_req_schema2 SET SCHEMA test_s_dep;  -- allowed
SELECT test_s_dep.dep_req1();
SELECT test_s_dep.dep_req2();
SELECT dep_req3();
SELECT dep_req3b();  -- fails
DROP EXTENSION test_ext_req_schema3;
ALTER EXTENSION test_ext_req_schema1 SET SCHEMA test_s_dep2;  -- now ok
SELECT test_s_dep2.dep_req1();
SELECT test_s_dep.dep_req2();
DROP EXTENSION test_ext_req_schema1 CASCADE;

-- Verify that name resolution during an extension script cannot be captured
-- by objects an unprivileged user planted in the extension's schema.
CREATE ROLE regress_ext_user;
CREATE SCHEMA test_overload;
GRANT CREATE, USAGE ON SCHEMA test_overload TO regress_ext_user;
-- As the unprivileged user, plant differently-typed siblings and the sole
-- definition of helper_only().
SET ROLE regress_ext_user;
CREATE FUNCTION test_overload.f(text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE FUNCTION test_overload.opimpl_bad(text, text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR test_overload.### (leftarg = text, rightarg = text,
                                   function = test_overload.opimpl_bad);
CREATE FUNCTION test_overload.helper_only(text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE FUNCTION test_overload.opimpl_only(text, text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR test_overload.@@@ (leftarg = text, rightarg = text,
                                   function = test_overload.opimpl_only);
CREATE FUNCTION test_overload.opimpl_vc(varchar, varchar) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR test_overload.<<< (leftarg = varchar, rightarg = varchar,
                                   function = test_overload.opimpl_vc);
RESET ROLE;
-- A schema outside the script's search path, holding a planted operator.
CREATE SCHEMA test_overload_other;
GRANT CREATE, USAGE ON SCHEMA test_overload_other TO regress_ext_user;
SET ROLE regress_ext_user;
CREATE OPERATOR test_overload_other.&&& (leftarg = text, rightarg = text,
                                         function = test_overload.opimpl_only);
RESET ROLE;
-- Installing the extension resolves f('abc') and the ### operator to the
-- extension's own (trusted) objects, not the planted ones.
CREATE EXTENSION test_ext_overload SCHEMA test_overload;
SELECT fn, op FROM test_overload.captured;
-- Outside of extension scripts, ordinary resolution rules are unchanged: the
-- same calls reach the planted objects.
SELECT test_overload.f('abc') AS fn,
       ('a' OPERATOR(test_overload.###) 'b') AS op;
-- When only an untrusted candidate exists, the script refuses to call it.
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload;  -- fails
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload VERSION '2.0';  -- fails
-- A planted operator named as COMMUTATOR is refused as well.
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload VERSION '3.0';  -- fails
-- Trusted candidate visible but arguments don't match: argument error, with
-- the untrusted candidate as a hint.
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload VERSION '4.0';  -- fails
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload VERSION '5.0';  -- fails
-- Untrusted candidate outside the search path: ordinary not-in-path error.
CREATE EXTENSION test_ext_overload_strict SCHEMA test_overload VERSION '6.0';  -- fails
DROP EXTENSION test_ext_overload;
DROP SCHEMA test_overload CASCADE;
DROP SCHEMA test_overload_other CASCADE;
DROP ROLE regress_ext_user;

-- A "superuser = false" script runs as the invoking user, so its objects are
-- owned by that role.  They must still be trusted, and another user's plant
-- must not be.
CREATE ROLE regress_ext_owner;
CREATE ROLE regress_ext_attacker;
DO $$ BEGIN
    EXECUTE format('GRANT CREATE ON DATABASE %I TO regress_ext_owner',
                   current_database());
END $$;
CREATE SCHEMA test_nosuper AUTHORIZATION regress_ext_owner;
GRANT CREATE, USAGE ON SCHEMA test_nosuper TO regress_ext_attacker;
-- Attacker plants a preferred-type (text) sibling of the extension's g().
SET ROLE regress_ext_attacker;
CREATE FUNCTION test_nosuper.g(text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
RESET ROLE;
-- The script runs as regress_ext_owner and must resolve g('abc') to its own
-- varchar function, not the attacker's text one.
SET ROLE regress_ext_owner;
CREATE EXTENSION test_ext_overload_nosuper SCHEMA test_nosuper;
SELECT fn FROM test_nosuper.captured_nosuper;
RESET ROLE;
-- An update run by another role (here the superuser) must still reach the
-- extension's own g(), which the install left owned by regress_ext_owner.
ALTER EXTENSION test_ext_overload_nosuper UPDATE TO '2.0';
SELECT fn FROM test_nosuper.updated;
DROP EXTENSION test_ext_overload_nosuper;
DROP SCHEMA test_nosuper CASCADE;
DO $$ BEGIN
    EXECUTE format('REVOKE CREATE ON DATABASE %I FROM regress_ext_owner',
                   current_database());
END $$;
DROP ROLE regress_ext_owner;
DROP ROLE regress_ext_attacker;

-- Resolution in a parallel worker must apply the same check, so
-- creating_extension must reach the worker.  Force wrap() into a worker with
-- debug_parallel_query.
CREATE ROLE regress_ext_attacker NOSUPERUSER;
CREATE SCHEMA test_parallel;
GRANT CREATE, USAGE ON SCHEMA test_parallel TO regress_ext_attacker;
SET ROLE regress_ext_attacker;
CREATE FUNCTION test_parallel.probe(text) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;
RESET ROLE;
SET debug_parallel_query = on;
CREATE EXTENSION test_ext_overload_parallel SCHEMA test_parallel;
RESET debug_parallel_query;
-- The worker resolved probe('x') to the extension's own probe(varchar), not
-- the planted probe(text).
SELECT who FROM test_parallel.captured;
DROP EXTENSION test_ext_overload_parallel;
DROP SCHEMA test_parallel CASCADE;
DROP ROLE regress_ext_attacker;

-- A plant in the extension's own schema must not shadow a required
-- extension's object, for any kind of reference: call, DDL by name, type,
-- relation, or a resolution cached before the script.
CREATE ROLE regress_ext_attacker;
CREATE ROLE regress_ext_reqowner;
DO $$ BEGIN
    EXECUTE format('GRANT CREATE ON DATABASE %I TO regress_ext_reqowner',
                   current_database());
END $$;
CREATE SCHEMA test_reqdep AUTHORIZATION regress_ext_reqowner;
CREATE SCHEMA test_req;
GRANT CREATE, USAGE ON SCHEMA test_req TO regress_ext_attacker;
-- The required extension is "superuser = false" and installed by an ordinary
-- role, so its objects are reachable only through required-extension
-- membership.
SET ROLE regress_ext_reqowner;
CREATE EXTENSION test_ext_overload_req_dep SCHEMA test_reqdep;
RESET ROLE;
SET ROLE regress_ext_attacker;
CREATE FUNCTION test_req.reqcall(int) RETURNS text
    AS $$ SELECT 'attacker'::text $$ LANGUAGE sql IMMUTABLE;
CREATE FUNCTION test_req.reqeq(int, int) RETURNS boolean
    AS $$ SELECT false $$ LANGUAGE sql IMMUTABLE;
CREATE OPERATOR test_req.=== (leftarg = integer, rightarg = integer,
                              function = test_req.reqeq);
-- Resolving to this domain would run pwn() with the script's privileges.
CREATE FUNCTION test_req.pwn(text) RETURNS boolean
    AS $$ BEGIN RAISE EXCEPTION 'attacker code executed'; END $$ LANGUAGE plpgsql;
CREATE DOMAIN test_req.reqdom AS text CHECK (test_req.pwn(VALUE));
CREATE TABLE test_req.reqtab(t text);
RESET ROLE;
-- Cache a resolution made outside any script, under the search_path the
-- script will pin; that plan must not be reused inside the script.
SET search_path = test_req, test_reqdep, pg_temp;
SELECT test_reqdep.reqplpgsql() AS warmed_outside_script;
RESET search_path;
CREATE EXTENSION test_ext_overload_req SCHEMA test_req;
-- Every reference resolved to the required extension's objects (in
-- test_reqdep), not the planted ones in test_req.
SELECT c.who, c.who_cached, n.nspname AS domain_schema
  FROM test_req.captured c
  JOIN pg_type t ON t.oid = c.dom
  JOIN pg_namespace n ON n.oid = t.typnamespace;
SELECT n.nspname AS operator_func_schema
  FROM pg_operator o
  JOIN pg_proc p ON p.oid = o.oprcode
  JOIN pg_namespace n ON n.oid = p.pronamespace
 WHERE o.oprname = '###' AND o.oprnamespace = 'test_req'::regnamespace;
SELECT 'test_reqdep.reqtab' AS tbl, count(*) FROM test_reqdep.reqtab
UNION ALL
SELECT 'test_req.reqtab', count(*) FROM test_req.reqtab;
SELECT n.nspname AS opfamily_member_schema
  FROM pg_amop a
  JOIN pg_opfamily f ON f.oid = a.amopfamily
  JOIN pg_operator o ON o.oid = a.amopopr
  JOIN pg_namespace n ON n.oid = o.oprnamespace
 WHERE f.opfname = 'reqfam';
-- Outside the script the cached plan is good again.
SET search_path = test_req, test_reqdep, pg_temp;
SELECT test_reqdep.reqplpgsql() AS after_script;
RESET search_path;
DROP EXTENSION test_ext_overload_req;
DROP EXTENSION test_ext_overload_req_dep;
DROP SCHEMA test_req CASCADE;
DROP SCHEMA test_reqdep CASCADE;
DO $$ BEGIN
    EXECUTE format('REVOKE CREATE ON DATABASE %I FROM regress_ext_reqowner',
                   current_database());
END $$;
DROP ROLE regress_ext_attacker;
DROP ROLE regress_ext_reqowner;
