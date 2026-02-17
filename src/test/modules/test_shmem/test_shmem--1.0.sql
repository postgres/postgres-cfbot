/* src/test/modules/test_shmem/test_shmem--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_shmem" to load this file. \quit


-- ===================================================================
-- Fixed-size shared memory structure
-- ===================================================================

CREATE FUNCTION get_test_shmem_attach_count()
RETURNS pg_catalog.int4 STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION test_shmem_resize_fixed(pg_catalog.int4)
RETURNS pg_catalog.void STRICT
AS 'MODULE_PATHNAME' LANGUAGE C;


-- ===================================================================
-- Resizable shared memory structure
-- ===================================================================

-- Function to resize the test structure in the shared memory
CREATE FUNCTION resizable_shmem_resize(new_entries integer)
RETURNS bool
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Function to write data to all entries in the test structure in shared memory
-- Writing all the entries makes sure that the memory is actually allocated and
-- mapped to the process, so that we can later measure the memory usage.
CREATE FUNCTION resizable_shmem_write(entry_value integer)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Function to verify that specified number of initial entries have expected value.
-- Reading all the entries makes sure that the memory is actually mapped to the
-- process, so that we can later measure the memory usage.
CREATE FUNCTION resizable_shmem_read(entry_count integer, entry_value integer)
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Function to report memory mapped against the main shared memory segment in
-- the backend where this function runs.
CREATE FUNCTION test_shmem_usage()
RETURNS bigint
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Function to get the shared memory page size
CREATE FUNCTION test_shmem_pagesize()
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Function to crash the backend by walking entries past the current size up to
-- the reserved maximum, reading or writing each one as decided by mode.
CREATE FUNCTION resizable_shmem_access_beyond_size(mode text)
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
