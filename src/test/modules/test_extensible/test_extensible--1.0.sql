-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_extensible" to load this file. \quit

CREATE OR REPLACE FUNCTION test_get_extensible_node_methods(text, bool) RETURNS text
  AS 'MODULE_PATHNAME', 'test_get_extensible_node_methods' LANGUAGE C;

CREATE OR REPLACE FUNCTION test_get_custom_scan_methods(text, bool) RETURNS text
  AS 'MODULE_PATHNAME', 'test_get_custom_scan_methods' LANGUAGE C;

CREATE OR REPLACE FUNCTION test_ext_node_callbacks(oid, int) RETURNS text
  AS 'MODULE_PATHNAME', 'test_ext_node_callbacks' LANGUAGE C STRICT;