/* src/test/modules/test_extensions/test_ext_overload_nosuper--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_nosuper" to load this file. \quit

-- The script runs as the invoking non-superuser, so g() is owned by that
-- role.  g('abc') must still resolve to it, not to a planted g(text).
CREATE FUNCTION @extschema@.g(varchar) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE;

CREATE TABLE @extschema@.captured_nosuper AS
    SELECT @extschema@.g('abc') AS fn;
