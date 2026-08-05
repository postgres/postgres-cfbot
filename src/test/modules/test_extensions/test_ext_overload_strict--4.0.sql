/* src/test/modules/test_extensions/test_ext_overload_strict--4.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- Trusted f(varchar) and planted f(text) are visible; f(1) matches neither,
-- so the error is about the argument types.
CREATE TABLE @extschema@.captured AS
    SELECT @extschema@.f(1) AS r;
