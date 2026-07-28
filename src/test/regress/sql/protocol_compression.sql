-- Allow all available compression algorithms
SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd,lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 and zstd (not supported) ***'
   \quit
\endif
SHOW protocol_backend_compression_allowed_algorithms;

-- Directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR
\getenv abs_builddir PG_ABS_BUILDDIR

-----------
-- Test protocol_backend_compression GUC
-----------

-- Force threshold to 0 to compress every message
SET protocol_backend_compression_threshold=0;

-- Disable compression without active compression
SET protocol_backend_compression = 'none';

-- Test lz4 -> none -> lz4
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';

-- Test lz4 -> none -> lz4 + using compressor
SET protocol_backend_compression = 'lz4';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'lz4';
SELECT 1;

-- Test zstd -> none -> zstd
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'zstd';

-- Test zstd -> none -> zstd + using compressor
SET protocol_backend_compression = 'zstd';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'zstd';
SELECT 1;

-- Test none -> zstd -> none
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'none';

-- Test none -> zstd -> none + using compressor
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'zstd';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;

-- Test none -> lz4 -> none
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';

-- Test none -> lz4 -> none + using compressor
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'lz4';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;

-- Disabling compression
SET protocol_backend_compression = 'none';

-- Test unsupported algorithm
SET protocol_backend_compression = 'gzip';

-- Test unrecognized algorithm
SET protocol_backend_compression = 'bogus';

-- Test none -> unsupported -> bogus
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'gzip';
SET protocol_backend_compression = 'bogus';

-- Test supported specifications
SET protocol_backend_compression = 'zstd';
SELECT 1;
SET protocol_backend_compression = 'zstd:5';
SELECT 1;
SET protocol_backend_compression = 'zstd:-10';
SELECT 1;
SET protocol_backend_compression = 'zstd:level=-10';
SELECT 1;
SET protocol_backend_compression = 'zstd:level=-10,workers=0';
SELECT 1;
SET protocol_backend_compression = 'zstd:level=-10,workers=0,long=1';
SELECT 1;
SET protocol_backend_compression = 'lz4';
SELECT 1;
SET protocol_backend_compression = 'lz4:5';
SELECT 1;
SET protocol_backend_compression = 'none';
SELECT 1;
SET protocol_backend_compression = 'none:0';
SELECT 1;

-- Test incorrect specifications
SET protocol_backend_compression = 'zstd:999';
SET protocol_backend_compression = 'lz4:999';
SET protocol_backend_compression = 'zstd:lvel=1';
SET protocol_backend_compression = 'zstd:workers=4';
SET protocol_backend_compression = 'zstd:long=3';
SET protocol_backend_compression = 'lz4:long=1';
SET protocol_backend_compression = 'lz4:workers=1';
SET protocol_backend_compression = 'none:long=1';
SET protocol_backend_compression = 'none:workers=1';
SET protocol_backend_compression = 'none:2';

-- Test compressor context teardown/setup
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'lz4';
SET protocol_backend_compression = 'none';
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'lz4';

-- Test enabling/disabling long distance
SET protocol_backend_compression = 'zstd:level=19,long=1';
SELECT 1;
SET protocol_backend_compression = 'zstd:level=19,long=0';
SELECT 1;

-- Test changing lz4 compression level
SET protocol_backend_compression = 'lz4:level=12';
SELECT 1;
SET protocol_backend_compression = 'lz4:level=2';
SELECT 1;

-----------
-- Test protocol_backend_compression_allowed_algorithms GUC
-----------

-- With only lz4 allowed, switching to zstd should fail
SET protocol_backend_compression_allowed_algorithms = 'lz4';
SHOW protocol_backend_compression_allowed_algorithms;
SET protocol_backend_compression = 'zstd';

-- Unsupported and unrecognized algorithms are rejected, without changing
-- the previous valid setting.
SET protocol_backend_compression_allowed_algorithms = 'gzip';
SET protocol_backend_compression_allowed_algorithms = 'bogus';
SHOW protocol_backend_compression_allowed_algorithms;

-- Disabling compression is always allowed
SET protocol_backend_compression_allowed_algorithms = 'none';
SET protocol_backend_compression_allowed_algorithms = '';
SHOW protocol_backend_compression_allowed_algorithms;
SET protocol_backend_compression = 'none';

-- With every algorithm forbidden, enabling compression should fail
SET protocol_backend_compression = 'zstd';
SET protocol_backend_compression = 'lz4';

-----------
-- Test large object with compression
-----------
SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SET protocol_backend_compression_threshold=0;
SET protocol_backend_compression='zstd';

\set filename :abs_srcdir '/data/tenk.data'

\lo_import :filename

\set newloid :LASTOID

-- just make sure \lo_export does not barf
\set filename :abs_builddir '/results/lotest_compression.txt'
\lo_export :newloid :filename
\lo_unlink :newloid
