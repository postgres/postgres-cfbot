--
-- Tests for FILEOPS UNDO rollback of direct file operations.
--
-- This test module provides SQL-callable wrappers around FileOps C functions
-- that have no DDL-level callers, enabling direct testing of UNDO rollback
-- for: FileOpsTruncate, FileOpsChmod, FileOpsLink, FileOpsSetXattr,
-- FileOpsRemoveXattr.
--
-- REQUIRES: UNDO subsystem (always active)
--

CREATE EXTENSION test_fileops;

-- ================================================================
-- Setup: create test files in the data directory
-- ================================================================

SELECT test_fileops_create_tempfile('test_fileops_a.dat') AS filepath_a \gset
SELECT test_fileops_create_tempfile('test_fileops_b.dat') AS filepath_b \gset

-- Record initial state (umask may affect mode)
SELECT test_fileops_file_size(:'filepath_a') AS initial_size;
SELECT test_fileops_get_mode(:'filepath_a') AS initial_mode;

-- ================================================================
-- Test 1: FileOpsTruncate UNDO - rollback restores original size
-- ================================================================

BEGIN;
SELECT test_fileops_truncate(:'filepath_a', 256);
SELECT test_fileops_file_size(:'filepath_a') AS size_during_txn;
ROLLBACK;

-- After rollback, UNDO should restore original size (1024)
SELECT test_fileops_file_size(:'filepath_a') AS size_after_rollback;

-- ================================================================
-- Test 2: FileOpsTruncate commit - change persists
-- ================================================================

BEGIN;
SELECT test_fileops_truncate(:'filepath_b', 512);
COMMIT;

SELECT test_fileops_file_size(:'filepath_b') AS size_after_commit;

-- ================================================================
-- Test 3: FileOpsChmod UNDO - rollback restores original permissions
-- ================================================================

-- Change to a known starting mode first (committed)
SELECT test_fileops_chmod(:'filepath_a', 420);  -- 0644 = 420 decimal
SELECT test_fileops_get_mode(:'filepath_a') AS mode_baseline;

BEGIN;
-- Change to 0600 = 384 decimal
SELECT test_fileops_chmod(:'filepath_a', 384);
SELECT test_fileops_get_mode(:'filepath_a') AS mode_during_txn;
ROLLBACK;

-- After rollback, UNDO should restore to 0644 = 420
SELECT test_fileops_get_mode(:'filepath_a') AS mode_after_rollback;

-- ================================================================
-- Test 4: FileOpsChmod commit - change persists
-- ================================================================

BEGIN;
SELECT test_fileops_chmod(:'filepath_a', 448);  -- 0700 = 448 decimal
COMMIT;

SELECT test_fileops_get_mode(:'filepath_a') AS mode_after_commit;

-- Restore for subsequent tests
SELECT test_fileops_chmod(:'filepath_a', 420);  -- 0644

-- ================================================================
-- Test 5: FileOpsLink UNDO - rollback removes the hard link
-- ================================================================

SELECT test_fileops_data_dir() || '/test_fileops_link.dat' AS linkpath \gset

BEGIN;
SELECT test_fileops_link(:'filepath_a', :'linkpath');
SELECT test_fileops_file_exists(:'linkpath') AS link_during_txn;
ROLLBACK;

-- After rollback, UNDO should remove the hard link
SELECT test_fileops_file_exists(:'linkpath') AS link_after_rollback;

-- ================================================================
-- Test 6: FileOpsLink commit - link persists
-- ================================================================

BEGIN;
SELECT test_fileops_link(:'filepath_a', :'linkpath');
COMMIT;

SELECT test_fileops_file_exists(:'linkpath') AS link_after_commit;

-- ================================================================
-- Test 7: FileOpsTruncate in subtransaction - ROLLBACK TO undoes it
-- ================================================================

-- Ensure file is 1024 bytes
SELECT test_fileops_file_size(:'filepath_a') AS size_before_test7;

BEGIN;
-- Outer truncate (should persist after commit)
SELECT test_fileops_truncate(:'filepath_a', 800);

SAVEPOINT sp1;
-- Inner truncate (should be rolled back)
SELECT test_fileops_truncate(:'filepath_a', 100);
SELECT test_fileops_file_size(:'filepath_a') AS size_in_savepoint;
ROLLBACK TO sp1;

-- After ROLLBACK TO, should be back to 800 (the outer truncate value)
SELECT test_fileops_file_size(:'filepath_a') AS size_after_rollback_to;
COMMIT;

-- After commit, the outer truncate to 800 should persist
SELECT test_fileops_file_size(:'filepath_a') AS size_after_commit;

-- ================================================================
-- Test 8: FileOpsChmod in subtransaction - ROLLBACK TO restores mode
-- ================================================================

-- Set a known mode baseline
SELECT test_fileops_chmod(:'filepath_a', 420);  -- 0644

BEGIN;
-- Outer chmod (persists)
SELECT test_fileops_chmod(:'filepath_a', 448);  -- 0700

SAVEPOINT sp1;
-- Inner chmod (rolled back)
SELECT test_fileops_chmod(:'filepath_a', 256);  -- 0400
SELECT test_fileops_get_mode(:'filepath_a') AS mode_in_savepoint;
ROLLBACK TO sp1;

-- After ROLLBACK TO, should be back to 0700 = 448
SELECT test_fileops_get_mode(:'filepath_a') AS mode_after_rollback_to;
COMMIT;

-- After commit, outer chmod (0700 = 448) persists
SELECT test_fileops_get_mode(:'filepath_a') AS mode_after_commit;

-- ================================================================
-- Test 9: FileOpsLink in subtransaction - ROLLBACK TO removes link
-- ================================================================

SELECT test_fileops_data_dir() || '/test_fileops_link2.dat' AS linkpath2 \gset

BEGIN;
SAVEPOINT sp1;
SELECT test_fileops_link(:'filepath_a', :'linkpath2');
SELECT test_fileops_file_exists(:'linkpath2') AS link2_in_savepoint;
ROLLBACK TO sp1;

-- Link should be removed by UNDO
SELECT test_fileops_file_exists(:'linkpath2') AS link2_after_rollback_to;
COMMIT;

SELECT test_fileops_file_exists(:'linkpath2') AS link2_after_commit;

-- ================================================================
-- Test 10: FileOpsSetXattr / FileOpsRemoveXattr UNDO
-- (skipped on platforms without xattr support)
-- ================================================================

-- Try to set an xattr; returns false if unsupported (ENOTSUP/EPERM/EACCES)
SELECT test_fileops_setxattr(:'filepath_a', 'user.test_key', 'initial_value')
   AS xattr_supported \gset

\if :xattr_supported

-- Test 10a: SetXattr UNDO - rollback removes newly-set xattr
BEGIN;
SELECT test_fileops_setxattr(:'filepath_a', 'user.rollback_test', 'will_vanish');
SELECT test_fileops_getxattr(:'filepath_a', 'user.rollback_test') AS xattr_during_txn;
ROLLBACK;

SELECT test_fileops_getxattr(:'filepath_a', 'user.rollback_test') AS xattr_after_rollback;

-- Test 10b: SetXattr overwrite UNDO - rollback restores old value
BEGIN;
SELECT test_fileops_setxattr(:'filepath_a', 'user.test_key', 'overwritten');
SELECT test_fileops_getxattr(:'filepath_a', 'user.test_key') AS xattr_overwritten;
ROLLBACK;

SELECT test_fileops_getxattr(:'filepath_a', 'user.test_key') AS xattr_restored;

-- Test 10c: RemoveXattr UNDO - rollback restores removed xattr
BEGIN;
SELECT test_fileops_removexattr(:'filepath_a', 'user.test_key');
SELECT test_fileops_getxattr(:'filepath_a', 'user.test_key') AS xattr_after_remove;
ROLLBACK;

SELECT test_fileops_getxattr(:'filepath_a', 'user.test_key') AS xattr_after_remove_rollback;

-- Test 10d: Xattr in subtransaction
BEGIN;
SAVEPOINT sp1;
SELECT test_fileops_setxattr(:'filepath_a', 'user.sub_key', 'sub_value');
SELECT test_fileops_getxattr(:'filepath_a', 'user.sub_key') AS xattr_in_savepoint;
ROLLBACK TO sp1;

SELECT test_fileops_getxattr(:'filepath_a', 'user.sub_key') AS xattr_after_sp_rollback;
COMMIT;

\else
SELECT 'xattr tests skipped (not supported on this platform/filesystem)' AS notice;
\endif

-- ================================================================
-- Cleanup
-- ================================================================

DROP EXTENSION test_fileops;
