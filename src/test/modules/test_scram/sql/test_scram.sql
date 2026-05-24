--
-- Direct tests for scram_parse_iterations() in src/common/scram-common.c.
--
CREATE EXTENSION test_scram;

-- Accepted: non-empty digit-only strings whose value is in [1, INT_MAX].
SELECT test_scram_parse_iterations('1');
SELECT test_scram_parse_iterations('4096');
SELECT test_scram_parse_iterations('999999999');
SELECT test_scram_parse_iterations('2147483647'); -- INT_MAX on every supported platform
SELECT test_scram_parse_iterations('0042');       -- leading zeros are still digits

-- Rejected: value below 1 (RFC 5802 requires a positive count).
SELECT test_scram_parse_iterations('0');

-- Rejected: anything that is not a pure digit string.
SELECT test_scram_parse_iterations('');           -- empty string
SELECT test_scram_parse_iterations('-1');         -- sign character
SELECT test_scram_parse_iterations('+1');         -- sign character
SELECT test_scram_parse_iterations(' 1');         -- leading whitespace
SELECT test_scram_parse_iterations('1 ');         -- trailing whitespace
SELECT test_scram_parse_iterations('1.0');        -- decimal point
SELECT test_scram_parse_iterations('1a');         -- digit then letter
SELECT test_scram_parse_iterations('a1');         -- letter then digit
SELECT test_scram_parse_iterations('abc');        -- all letters
SELECT test_scram_parse_iterations('0x10');       -- hex prefix is not digits-only

-- Rejected: out-of-range values that would otherwise narrow to a bogus int.
SELECT test_scram_parse_iterations('2147483648'); -- INT_MAX + 1
SELECT test_scram_parse_iterations('9999999999'); -- > INT_MAX on every supported platform
SELECT test_scram_parse_iterations('99999999999999999999'); -- overflows long on every supported platform
