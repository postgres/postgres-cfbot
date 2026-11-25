-- This test is about EUC_KR encoding, chosen as perhaps the most prevalent
-- non-UTF8, multibyte encoding as of 2026-01.  Since UTF8 can represent all
-- of EUC_KR, also run the test in UTF8.
SELECT getdatabaseencoding() NOT IN ('EUC_KR', 'UTF8') AS skip_test \gset
\if :skip_test
\quit
\endif

-- Exercise is_multibyte_char_in_char (non-UTF8) slow path.
SELECT POSITION(
	convert_from('\xbcf6c7d0', 'EUC_KR') IN
	convert_from('\xb0fac7d02c20bcf6c7d02c20b1e2bcfa2c20bbee', 'EUC_KR'));

-- Maximum number of bytes per character.
SELECT pg_encoding_max_length(pg_char_to_encoding('EUC_KR'));

-- Code set 0 (ASCII) characters take one byte and code set 1 (KS X 1001)
-- characters take two.  Count, search and match a string mixing both.
SELECT char_length(convert_from('\x41b0fac7d0', 'EUC_KR'));
SELECT POSITION(
	convert_from('\xc7d0', 'EUC_KR') IN
	convert_from('\x41b0fac7d0', 'EUC_KR'));
SELECT regexp_replace(convert_from('\x41b0fac7d0', 'EUC_KR'), '[^A]', 'x', 'g');

-- Code sets 2 and 3 are not defined for EUC_KR, so SS2 (0x8e) and SS3 (0x8f)
-- are not valid lead bytes.
SELECT convert_from('\x8ea1', 'EUC_KR');
SELECT convert_from('\x8fa1a1', 'EUC_KR');
