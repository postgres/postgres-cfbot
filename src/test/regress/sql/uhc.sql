-- This test is about UHC (Windows-949 / CP949) encoding.  UHC is a
-- client-only encoding, so exercise pg_uhc_verifychar() via convert_from()
-- in a UTF8 database.
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

-- valid: EUC_KR-compatible Hangul (U+AC00 "가")
SELECT encode(convert_to(convert_from('\xb0a1', 'UHC'), 'UTF8'), 'hex');

-- valid: CP949 lead/trail boundary values
SELECT encode(convert_to(convert_from('\x8141', 'UHC'), 'UTF8'), 'hex');	-- trail 0x41
SELECT encode(convert_to(convert_from('\x815a', 'UHC'), 'UTF8'), 'hex');	-- trail 0x5A
SELECT encode(convert_to(convert_from('\x8161', 'UHC'), 'UTF8'), 'hex');	-- trail 0x61
SELECT encode(convert_to(convert_from('\x817a', 'UHC'), 'UTF8'), 'hex');	-- trail 0x7A
SELECT encode(convert_to(convert_from('\x8181', 'UHC'), 'UTF8'), 'hex');	-- trail 0x81
SELECT encode(convert_to(convert_from('\x81fe', 'UHC'), 'UTF8'), 'hex');	-- trail 0xFE
SELECT encode(convert_to(convert_from('\xc7a1', 'UHC'), 'UTF8'), 'hex');	-- high lead 0xC7
SELECT encode(convert_to(convert_from('\xfda1', 'UHC'), 'UTF8'), 'hex');	-- high lead 0xFD
SELECT encode(convert_to(convert_from('\xfea1', 'UHC'), 'UTF8'), 'hex');	-- high lead 0xFE (upper boundary)

-- invalid lead byte (0x80 and 0xFF are unused in CP949)
SELECT convert_from('\x8041', 'UHC');
SELECT convert_from('\xff41', 'UHC');

-- invalid trail byte
SELECT convert_from('\x8140', 'UHC');	-- 0x40
SELECT convert_from('\x815b', 'UHC');	-- 0x5B
SELECT convert_from('\x8160', 'UHC');	-- 0x60
SELECT convert_from('\x817b', 'UHC');	-- 0x7B
SELECT convert_from('\x8180', 'UHC');	-- 0x80
SELECT convert_from('\x81ff', 'UHC');	-- 0xFF
SELECT convert_from('\x8d20', 'UHC');	-- NONUTF8_INVALID sentinel pair

-- truncated two-byte character
SELECT convert_from('\x81', 'UHC');

-- invalid trail byte 0x00 (NUL); the one trail the old verifier also rejected
SELECT convert_from('\x8100', 'UHC');	-- trail 0x00
