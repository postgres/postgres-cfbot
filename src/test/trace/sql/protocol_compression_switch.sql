-- Test compression algorithm switch behavior

SET protocol_backend_compression_allowed_algorithms = 'zstd,lz4';
SELECT current_setting('protocol_backend_compression_allowed_algorithms')!='zstd,lz4' AS skip_test \gset
\if :skip_test
   \echo '*** skipping protocol compression tests with lz4 and zstd (not supported) ***'
   \quit
\endif

-- Test compression algorithm change with an opened frame
SET protocol_backend_compression='zstd';
SET protocol_backend_compression_threshold=0;
SELECT 1;
SET protocol_backend_compression='lz4';
SELECT 1;
SET protocol_backend_compression='zstd';

RESET protocol_backend_compression_number_messages;
RESET protocol_backend_compression;
