SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif

SELECT U&'\0061\0308bc' <> U&'\00E4bc' COLLATE "C" AS sanity_check;

SELECT unicode_version() IS NOT NULL;
SELECT unicode_assigned(U&'abc');
SELECT unicode_assigned(U&'abc\+10FFFF');
SELECT unicode_assigned('abc');

SELECT normalize('');
SELECT normalize(U&'\0061\0308\24D1c') = U&'\00E4\24D1c' COLLATE "C" AS test_default;
SELECT normalize(U&'\0061\0308\24D1c', NFC) = U&'\00E4\24D1c' COLLATE "C" AS test_nfc;
SELECT normalize(U&'\00E4bc', NFC) = U&'\00E4bc' COLLATE "C" AS test_nfc_idem;
SELECT normalize(U&'\00E4\24D1c', NFD) = U&'\0061\0308\24D1c' COLLATE "C" AS test_nfd;
SELECT normalize(U&'\0061\0308\24D1c', NFKC) = U&'\00E4bc' COLLATE "C" AS test_nfkc;
SELECT normalize(U&'\00E4\24D1c', NFKD) = U&'\0061\0308bc' COLLATE "C" AS test_nfkd;
SELECT normalize('abc') = 'abc' COLLATE "C" AS test_ascii_idem;
-- Use stored values so this check is not folded to a constant.
CREATE TABLE unicode_ascii_input (val text);
INSERT INTO unicode_ascii_input VALUES
    (''), ('abc'), (repeat('a', 64)), (repeat('b', 5000));
SELECT bool_and(normalize(val) = val COLLATE "C") AS test_ascii_idem_column,
    count(pg_column_compression(val)) AS num_compressed
FROM unicode_ascii_input;
DROP TABLE unicode_ascii_input;

SELECT "normalize"('abc', 'def');  -- run-time error
SELECT "normalize"('abc', 'NFCx');  -- run-time error
SELECT "normalize"(U&'\0061\0308', 'nFc') = U&'\00E4' COLLATE "C" AS test_form_case;

SELECT U&'\00E4\24D1c' IS NORMALIZED AS test_default;
SELECT U&'\00E4\24D1c' IS NFC NORMALIZED AS test_nfc;
SELECT 'abc' IS NORMALIZED AS test_ascii;

SELECT num, val,
    val IS NFC NORMALIZED AS NFC,
    val IS NFD NORMALIZED AS NFD,
    val IS NFKC NORMALIZED AS NFKC,
    val IS NFKD NORMALIZED AS NFKD
FROM
  (VALUES (1, U&'\00E4bc'),
          (2, U&'\0061\0308bc'),
          (3, U&'\00E4\24D1c'),
          (4, U&'\0061\0308\24D1c'),
          (5, '')) vals (num, val)
ORDER BY num;

SELECT is_normalized('abc', 'def');  -- run-time error
SELECT is_normalized('abc', 'NFKCx');  -- run-time error

-- Exercise the ASCII fast-path's chunk/remainder boundary handling: a
-- non-NFC-normalized codepoint sequence must still be detected as such
-- regardless of how much pure-ASCII padding surrounds it.
SELECT len,
    (repeat('a', len) || U&'\0061\0308') IS NORMALIZED AS non_nfc_at_end,
    (U&'\0061\0308' || repeat('a', len)) IS NORMALIZED AS non_nfc_at_start
FROM generate_series(0, 40) AS len
ORDER BY len;

-- normalize() and unicode_assigned() skip leading ASCII too, so check that
-- they give the right answer whatever its length.  A combining mark right
-- after the ASCII must still compose with its last character.  This lists
-- only the lengths that give a wrong answer.
SELECT len
FROM generate_series(0, 40) AS len
WHERE normalize(repeat('a', len) || U&'\0061\0308', NFC)
        <> (repeat('a', len) || U&'\00E4') COLLATE "C"
   OR normalize(repeat('a', len) || U&'\00E4' || repeat('b', len), NFD)
        <> (repeat('a', len) || U&'\0061\0308' || repeat('b', len)) COLLATE "C"
   OR normalize(repeat('a', len) || U&'\00E4' || repeat('b', len), NFC)
        <> (repeat('a', len) || U&'\00E4' || repeat('b', len)) COLLATE "C"
   OR unicode_assigned(repeat('a', len) || U&'\+10FFFF')
   OR NOT unicode_assigned(repeat('a', len) || U&'\00E4' || repeat('b', len));

-- Hangul NFC recomposition tests
-- L+V -> LV composition (first and last)
SELECT normalize(U&'\1100\1161', NFC) = U&'\AC00' COLLATE "C" AS hangul_lv_first;
SELECT normalize(U&'\1112\1175', NFC) = U&'\D788' COLLATE "C" AS hangul_lv_last;
-- LV+T -> LVT composition
SELECT normalize(U&'\AC00\11A8', NFC) = U&'\AC01' COLLATE "C" AS hangul_lvt_first_t;
SELECT normalize(U&'\AC00\11C2', NFC) = U&'\AC1B' COLLATE "C" AS hangul_lvt_last_t;
SELECT normalize(U&'\D788\11A8', NFC) = U&'\D789' COLLATE "C" AS hangul_lvt_last_lv;
-- L+V+T -> LVT composition
SELECT normalize(U&'\1100\1161\11A8', NFC) = U&'\AC01' COLLATE "C" AS hangul_full_lvt;
SELECT normalize(U&'\1112\1175\11C2', NFC) = U&'\D7A3' COLLATE "C" AS hangul_full_lvt;
-- TBASE invalid T syllable
SELECT normalize(U&'\AC00\11A7', NFC) = U&'\AC00\11A7' COLLATE "C" AS hangul_tbase_not_combined;
SELECT normalize(U&'\1100\1161\11A7', NFC) = U&'\AC00\11A7' COLLATE "C" AS hangul_lv_tbase_separate;

-- Hangul NFD decomposition tests
SELECT normalize(U&'\AC00', NFD) = U&'\1100\1161' COLLATE "C" AS hangul_nfd_lv;
SELECT normalize(U&'\AC01', NFD) = U&'\1100\1161\11A8' COLLATE "C" AS hangul_nfd_lvt;
SELECT normalize(U&'\D7A3', NFD) = U&'\1112\1175\11C2' COLLATE "C" AS hangul_nfd_last;
