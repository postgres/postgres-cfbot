/* src/test/modules/test_extensions/test_ext_overload_parallel--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_parallel" to load this file. \quit

-- wrap() is parallel safe and its body is parsed at run time, so under
-- debug_parallel_query the worker resolves probe('x').  It must reach
-- probe(varchar), not a planted probe(text).
CREATE FUNCTION @extschema@.probe(varchar) RETURNS text
    AS $$ SELECT 'extension'::text $$ LANGUAGE sql IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION @extschema@.wrap() RETURNS text
    LANGUAGE plpgsql PARALLEL SAFE AS $$ BEGIN RETURN probe('x'); END $$;

CREATE TABLE @extschema@.captured AS SELECT @extschema@.wrap() AS who;
