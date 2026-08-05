/* src/test/modules/test_extensions/test_ext_overload_strict--10.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- As 8.0, unqualified: resolved through search_path by
-- TypenameGetTypidExtended.
CREATE FUNCTION @extschema@.g(planted_dom) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE;
