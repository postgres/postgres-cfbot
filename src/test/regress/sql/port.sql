--
-- Test port code.
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_pg_threads_ext()
    RETURNS void
    AS :'regresslib'
    LANGUAGE C;

SELECT test_pg_threads_ext();
