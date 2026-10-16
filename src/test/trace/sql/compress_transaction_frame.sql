-- Test compress transaction frame behavior

SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd,lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping compress transaction frame tests with zstd (not supported) ***'
   \quit
\endif

SET protocol_backend_compression='zstd';
SET protocol_backend_compression_transaction_frame=true;

-- No compression triggered, frame shouldn't be closed
SELECT 1;

-- Frame should be closed on COMMIT
BEGIN;
SELECT repeat('a', 80);
COMMIT;

-- Frame should be closed on ROLLBACK
BEGIN;
SELECT repeat('a', 80);
ROLLBACK;

-- Implicit transaction should also close the frame
SELECT repeat('a', 80);

-- After a closed frame, sending uncompressed messages shouldn't generate any
-- CompressedMessages
SELECT 1;

-- This should be sent in a single compressed frame
SET protocol_backend_compression_transaction_frame=false;
SELECT 1;
BEGIN;
SELECT repeat('a', 80);
COMMIT;
BEGIN;
SELECT repeat('a', 80);
ROLLBACK;
SELECT repeat('a', 80);

RESET protocol_backend_compression;
RESET protocol_backend_compression_transaction_frame;
