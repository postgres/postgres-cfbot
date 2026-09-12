/* src/test/modules/test_extensions/test_ext_overload_strict--5.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- As in 4.0, for an operator: integer operands match neither ###.
CREATE TABLE @extschema@.captured AS
    SELECT (1 OPERATOR(@extschema@.###) 2) AS r;
