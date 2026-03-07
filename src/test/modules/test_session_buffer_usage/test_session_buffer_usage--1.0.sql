/* src/test/modules/test_session_buffer_usage/test_session_buffer_usage--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_session_buffer_usage" to load this file. \quit

CREATE FUNCTION test_session_buffer_usage(
    OUT shared_blks_hit bigint,
    OUT shared_blks_read bigint,
    OUT shared_blks_dirtied bigint,
    OUT shared_blks_written bigint,
    OUT local_blks_hit bigint,
    OUT local_blks_read bigint,
    OUT local_blks_dirtied bigint,
    OUT local_blks_written bigint,
    OUT temp_blks_read bigint,
    OUT temp_blks_written bigint,
    OUT shared_blk_read_time double precision,
    OUT shared_blk_write_time double precision,
    OUT local_blk_read_time double precision,
    OUT local_blk_write_time double precision,
    OUT temp_blk_read_time double precision,
    OUT temp_blk_write_time double precision
)
RETURNS record
AS 'MODULE_PATHNAME', 'test_session_buffer_usage'
LANGUAGE C PARALLEL RESTRICTED;

CREATE FUNCTION test_session_buffer_usage_reset()
RETURNS void
AS 'MODULE_PATHNAME', 'test_session_buffer_usage_reset'
LANGUAGE C PARALLEL RESTRICTED;
