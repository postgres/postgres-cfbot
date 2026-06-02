-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION pg_buffercache UPDATE TO '1.8'" to load this file. \quit

-- Register the new functions.
CREATE OR REPLACE FUNCTION pg_buffercache_partitions()
RETURNS SETOF RECORD
AS 'MODULE_PATHNAME', 'pg_buffercache_partitions'
LANGUAGE C PARALLEL SAFE;

-- Create a view for convenient access.
CREATE VIEW pg_buffercache_partitions AS
	SELECT P.* FROM pg_buffercache_partitions() AS P
	(partition integer,			-- partition index
	 num_buffers integer,		-- number of buffers in the partition
	 first_buffer integer,		-- first buffer of partition
	 last_buffer integer);		-- last buffer of partition

-- Don't want these to be available to public.
REVOKE ALL ON FUNCTION pg_buffercache_partitions() FROM PUBLIC;
REVOKE ALL ON pg_buffercache_partitions FROM PUBLIC;

GRANT EXECUTE ON FUNCTION pg_buffercache_partitions() TO pg_monitor;
GRANT SELECT ON pg_buffercache_partitions TO pg_monitor;
