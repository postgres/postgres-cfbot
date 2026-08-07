/* src/test/modules/dummy_table_am/dummy_table_am--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION dummy_table_am" to load this file. \quit

CREATE FUNCTION dthandler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Access method
CREATE ACCESS METHOD dummy_table_am TYPE TABLE HANDLER dthandler;
COMMENT ON ACCESS METHOD dummy_table_am IS 'dummy table access method';
