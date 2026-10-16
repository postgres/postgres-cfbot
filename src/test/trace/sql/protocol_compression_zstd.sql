-- Test protocol compression with zstd

SET protocol_backend_compression_allowed_algorithms = 'zstd';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with zstd (not supported) ***'
   \quit
\endif

-------------------------
-- Enable/Disable compression
-------------------------

-- While we are below the compression threshold, message bytes are stored in
-- pqcompress' buffer. Setting compression to none should flush stored messages
-- immediately
SET protocol_backend_compression='zstd';
SET protocol_backend_compression_threshold=100;
\startpipeline
SELECT 1 \parse one
SET protocol_backend_compression='none';
\endpipeline

-- Test enabling compression within a transaction
SET protocol_backend_compression_threshold=0;
BEGIN;
SET LOCAL protocol_backend_compression='zstd';
SELECT 1;
COMMIT;

-- Test enabling compression within an implicit transaction
\startpipeline
SET LOCAL protocol_backend_compression='zstd';
SELECT 1;
\endpipeline

-------------------------
-- Test Simple/Extended/Copy Queries
-------------------------

SET protocol_backend_compression='zstd';
SET protocol_backend_compression_threshold=0;

-- Check simple query
SELECT 1;

-- Check extended query
SELECT $1::text, '42', $1::numeric, interval '1 sec' \bind 1 \g

-- Check copy
\copy (SELECT *, repeat(' ', 80) FROM generate_series(1, 3)) to stdout;

-- Check Notify messages are compressed
LISTEN test_zstd;
NOTIFY test_zstd, 'test';
UNLISTEN test_zstd;

-- Removing allowed algorithm should leave the current compression
SET protocol_backend_compression_allowed_algorithms = '';
SELECT 1;
SET protocol_backend_compression_allowed_algorithms = 'zstd';

-- A flush request should force the uncompressed messages to be sent
-- immediately
\startpipeline
SELECT 1 \parse test
\flushrequest
\flush
\getresults
SELECT repeat('a', 100);
\endpipeline

-- We need to temporarily disable debug_parallel_query here. When the
-- error is triggered within a parallel worker, no data rows will be
-- sent which will lead to a different protocol trace.
SET debug_parallel_query=false;

-- Check triggering an error
-- Stay below the compression threshold
SET protocol_backend_compression_threshold=1000;
select 1 / i FROM generate_series(2, 0, -1) AS a(i);
-- Cross the compression threshold
SET protocol_backend_compression_threshold=5;
select 1 / i FROM generate_series(2, 0, -1) AS a(i);

RESET debug_parallel_query;

-- Test zstd -> none -> zstd with an opened frame
SET protocol_backend_compression='none';
SELECT 1;
SET protocol_backend_compression='zstd';

-- CompressedMessages should be sent if compress_number_messages threshold is
-- reached
SET protocol_backend_compression_number_messages=5;
SELECT * FROM generate_series(1, 10);
RESET protocol_backend_compression_number_messages;

-------------------------
-- Transaction frame
-------------------------
SET protocol_backend_compression_transaction_frame=true;
SET protocol_backend_compression_threshold=50;

-- No compression triggered, frame shouldn't be closed
SELECT 1;

-- Frame should be closed on COMMIT
BEGIN;
SELECT repeat('a', 40);
COMMIT;

-- Frame should be closed on ROLLBACK
BEGIN;
SELECT repeat('a', 40);
ROLLBACK;

-- Frame should be closed after an implicit transaction
SELECT repeat('a', 40);

-- After a closed frame, sending uncompressed messages shouldn't generate any
-- CompressedMessages
SELECT 1;

-- Frame should be left opened for the 2 batches of CompressedMessages
SET protocol_backend_compression_number_messages=5;
SELECT * FROM generate_series(1, 10);
RESET protocol_backend_compression_number_messages;

-- This should be sent in a single compressed frame
SET protocol_backend_compression_transaction_frame=false;
SELECT 1;
BEGIN;
SELECT repeat('a', 40);
COMMIT;
BEGIN;
SELECT repeat('a', 40);
ROLLBACK;
SELECT repeat('a', 40);

RESET protocol_backend_compression_threshold;
RESET protocol_backend_compression_number_messages;
RESET protocol_backend_compression_transaction_frame;
RESET protocol_backend_compression;
