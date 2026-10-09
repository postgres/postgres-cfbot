/* src/test/modules/test_extensions/test_ext_overload--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload" to load this file. \quit

-- f() and ### take varchar.  Resolving f('abc') during this script must
-- reach them, not a planted f(text) sibling.
CREATE FUNCTION @extschema@.f(varchar) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE;

CREATE FUNCTION @extschema@.opimpl(varchar, varchar) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE;

CREATE OPERATOR @extschema@.### (leftarg = varchar, rightarg = varchar,
                                 function = @extschema@.opimpl);

CREATE TABLE @extschema@.captured AS
    SELECT @extschema@.f('abc') AS fn,
           ('a' OPERATOR(@extschema@.###) 'b') AS op;
