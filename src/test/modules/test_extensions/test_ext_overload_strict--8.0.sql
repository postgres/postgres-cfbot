/* src/test/modules/test_extensions/test_ext_overload_strict--8.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- A planted domain reached by its schema-qualified name in a signature.
CREATE FUNCTION @extschema@.g(@extschema@.planted_dom) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE;
