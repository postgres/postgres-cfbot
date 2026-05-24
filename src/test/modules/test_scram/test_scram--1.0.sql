/* src/test/modules/test_scram/test_scram--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_scram" to load this file. \quit

--
-- test_scram_parse_iterations(text) -> text
--
-- Returns the parsed iteration count as text, or 'invalid' if the
-- parser rejects the input.  Used to exercise scram_parse_iterations()
-- in src/common/scram-common.c.
--
CREATE FUNCTION test_scram_parse_iterations(IN input text)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
