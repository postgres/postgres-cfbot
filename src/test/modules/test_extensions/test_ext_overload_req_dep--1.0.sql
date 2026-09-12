/* src/test/modules/test_extensions/test_ext_overload_req_dep--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_req_dep" to load this file. \quit

-- Objects the dependent extension's script references by unqualified name.
CREATE FUNCTION @extschema@.reqcall(int) RETURNS text
    AS $$ SELECT 'required'::text $$ LANGUAGE sql IMMUTABLE;

CREATE FUNCTION @extschema@.reqeq(int, int) RETURNS boolean
    AS $$ SELECT true $$ LANGUAGE sql IMMUTABLE;

CREATE DOMAIN @extschema@.reqdom AS text;

CREATE TABLE @extschema@.reqtab(t text);

-- Parsed at run time, so a call before the dependent script caches a
-- resolution made without trust checks.
CREATE FUNCTION @extschema@.reqplpgsql() RETURNS text
    LANGUAGE plpgsql AS $$ BEGIN RETURN reqcall(1); END $$;

CREATE OPERATOR @extschema@.=== (leftarg = integer, rightarg = integer,
                                 function = @extschema@.reqeq);
