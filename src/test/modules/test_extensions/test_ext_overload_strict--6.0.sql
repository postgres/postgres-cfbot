/* src/test/modules/test_extensions/test_ext_overload_strict--6.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- The only &&& is outside the search path; report that, not trust.
CREATE TABLE @extschema@.captured AS
    SELECT ('a' &&& 'b') AS r;
