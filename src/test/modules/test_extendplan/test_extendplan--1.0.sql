/* src/test/modules/test_extendplan/test_extendplan--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_extendplan" to load this file. \quit

CREATE FUNCTION test_extendplan_reset() RETURNS void
  AS 'MODULE_PATHNAME', 'test_extendplan_reset' LANGUAGE C STRICT;

CREATE FUNCTION test_extendplan_summary(OUT grouped_rels_checked bool,
  OUT unique_rels_checked bool, OUT mismatches int8)
  AS 'MODULE_PATHNAME', 'test_extendplan_summary' LANGUAGE C STRICT;
