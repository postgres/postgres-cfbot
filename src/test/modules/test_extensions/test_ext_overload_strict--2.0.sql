/* src/test/modules/test_extensions/test_ext_overload_strict--2.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- As for helper_only() in 1.0, but for an operator: the only definition of
-- @@@ is one an unprivileged user planted, so the script must refuse it.
CREATE TABLE @extschema@.captured AS
    SELECT ('a' OPERATOR(@extschema@.@@@) 'b') AS r;
