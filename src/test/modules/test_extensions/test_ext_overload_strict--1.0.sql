/* src/test/modules/test_extensions/test_ext_overload_strict--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- helper_only() exists only as a planted definition, so the script must
-- fail with "function does not exist".
CREATE TABLE @extschema@.captured AS
    SELECT @extschema@.helper_only('abc') AS r;
