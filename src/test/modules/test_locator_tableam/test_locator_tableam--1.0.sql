/* src/test/modules/test_locator_tableam/test_locator_tableam--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_locator_tableam" to load this file. \quit

CREATE FUNCTION heap_noretain_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD heap_noretain TYPE TABLE HANDLER heap_noretain_handler;
COMMENT ON ACCESS METHOD heap_noretain IS 'heap that does not retain old row versions';

CREATE FUNCTION heap_wide_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD heap_wide TYPE TABLE HANDLER heap_wide_handler;
COMMENT ON ACCESS METHOD heap_wide IS 'heap with a locator wider than a TID';

CREATE FUNCTION heap_bigoffset_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD heap_bigoffset TYPE TABLE HANDLER heap_bigoffset_handler;
COMMENT ON ACCESS METHOD heap_bigoffset IS 'heap with offsets too large for a TID bitmap';

CREATE FUNCTION heap_orrecheck_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME'
LANGUAGE C;

CREATE ACCESS METHOD heap_orrecheck TYPE TABLE HANDLER heap_orrecheck_handler;
COMMENT ON ACCESS METHOD heap_orrecheck IS 'heap whose bitmap unions always need a recheck';
