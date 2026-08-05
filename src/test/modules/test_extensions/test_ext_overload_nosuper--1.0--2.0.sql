/* src/test/modules/test_extensions/test_ext_overload_nosuper--1.0--2.0.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION test_ext_overload_nosuper UPDATE" to load this file. \quit

-- g() belongs to this extension but is owned by the non-superuser who
-- installed 1.0, so an update run by any other role reaches it only by
-- extension membership.
CREATE TABLE @extschema@.updated AS SELECT g('abc') AS fn;
