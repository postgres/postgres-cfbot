--
-- int16_numeric.sql
--
-- Regression tests for casts between int16 (128-bit signed integer) and numeric.
-- Tests cover: round-trip conversions, edge cases (0, ±1, MIN, MAX, powers of
-- 10), fractional rounding behavior, NaN/Infinity rejection, overflow detection,
-- NULL handling, and cast usage in INSERT/SELECT contexts.
--

--
-- int16 → numeric cast (implicit)
--

-- Basic values
SELECT '0'::int16::numeric;
SELECT '1'::int16::numeric;
SELECT '-1'::int16::numeric;
SELECT '42'::int16::numeric;
SELECT '-42'::int16::numeric;

-- INT128_MAX and INT128_MIN
SELECT '170141183460469231731687303715884105727'::int16::numeric;  -- INT16_MAX
SELECT '-170141183460469231731687303715884105728'::int16::numeric; -- INT16_MIN

-- Powers of 10 (testing digit boundaries)
SELECT '1000000000000000000'::int16::numeric;      -- 10^18
SELECT '10000000000000000000'::int16::numeric;     -- 10^19
SELECT '100000000000000000000000000000000000000'::int16::numeric; -- 10^38

-- Values around int64 boundaries
SELECT '9223372036854775807'::int16::numeric;  -- INT64_MAX
SELECT '-9223372036854775808'::int16::numeric; -- INT64_MIN
SELECT '9223372036854775808'::int16::numeric;  -- INT64_MAX + 1
SELECT '-9223372036854775809'::int16::numeric; -- INT64_MIN - 1

-- 2^64
SELECT '18446744073709551616'::int16::numeric;
SELECT '-18446744073709551616'::int16::numeric;

-- Use int16_numeric() function directly
SELECT int16_numeric('123'::int16);
SELECT int16_numeric('-456'::int16);

--
-- numeric → int16 cast (assignment)
--

-- Basic values
SELECT '0'::numeric::int16;
SELECT '1'::numeric::int16;
SELECT '-1'::numeric::int16;
SELECT '42'::numeric::int16;
SELECT '-42'::numeric::int16;

-- INT128_MAX and INT128_MIN
SELECT '170141183460469231731687303715884105727'::numeric::int16;  -- INT16_MAX
SELECT '-170141183460469231731687303715884105728'::numeric::int16; -- INT16_MIN

-- Powers of 10
SELECT '1000000000000000000'::numeric::int16;      -- 10^18
SELECT '10000000000000000000'::numeric::int16;     -- 10^19
SELECT '100000000000000000000000000000000000000'::numeric::int16; -- 10^38

-- Use numeric_int16() function directly
SELECT numeric_int16('789'::numeric);
SELECT numeric_int16('-321'::numeric);

--
-- Fractional rounding (matches int8 behavior: round-half-away-from-zero)
--

SELECT '1.4'::numeric::int16;   -- rounds to 1
SELECT '1.5'::numeric::int16;   -- rounds to 2
SELECT '1.6'::numeric::int16;   -- rounds to 2
SELECT '-1.4'::numeric::int16;  -- rounds to -1
SELECT '-1.5'::numeric::int16;  -- rounds to -2
SELECT '-1.6'::numeric::int16;  -- rounds to -2
SELECT '2.5'::numeric::int16;   -- rounds to 3 (round half away from zero)
SELECT '-2.5'::numeric::int16;  -- rounds to -3
SELECT '0.4'::numeric::int16;   -- rounds to 0
SELECT '0.5'::numeric::int16;   -- rounds to 1
SELECT '-0.5'::numeric::int16;  -- rounds to -1
SELECT '99999999999999999999.5'::numeric::int16;  -- rounds to 10^20

--
-- Round-trip: int16 → numeric → int16
--

SELECT '170141183460469231731687303715884105727'::int16::numeric::int16;  -- MAX
SELECT '-170141183460469231731687303715884105728'::int16::numeric::int16; -- MIN
SELECT '9223372036854775807'::int16::numeric::int16;  -- INT64_MAX
SELECT '18446744073709551616'::int16::numeric::int16; -- 2^64
SELECT '123456789012345678901234567890'::int16::numeric::int16;

--
-- Round-trip: numeric → int16 → numeric
--

SELECT '170141183460469231731687303715884105727'::numeric::int16::numeric;
SELECT '-170141183460469231731687303715884105728'::numeric::int16::numeric;
SELECT '99999999999999999999999999999999'::numeric::int16::numeric;

--
-- NaN and Infinity (should error, matching int8 behavior)
--

SELECT 'NaN'::numeric::int16;
SELECT 'Infinity'::numeric::int16;
SELECT '-Infinity'::numeric::int16;

--
-- Overflow: values outside the int16 range (should error)
--

SELECT '170141183460469231731687303715884105728'::numeric::int16;  -- MAX + 1
SELECT '-170141183460469231731687303715884105729'::numeric::int16; -- MIN - 1
SELECT '999999999999999999999999999999999999999999999999'::numeric::int16; -- way too big

--
-- NULL handling
--

SELECT NULL::int16::numeric;
SELECT NULL::numeric::int16;

--
-- Cast usage in INSERT/SELECT
--

CREATE TABLE int16_numeric_test (
    id serial PRIMARY KEY,
    big_val int16,
    num_val numeric
);

-- Insert int16 values, cast to numeric on retrieval
INSERT INTO int16_numeric_test (big_val) VALUES
    ('1'),
    ('-1'),
    ('170141183460469231731687303715884105727'),
    ('-170141183460469231731687303715884105728'),
    ('9223372036854775807');

SELECT id, big_val::numeric FROM int16_numeric_test ORDER BY id;

-- Insert numeric values, cast to int16
INSERT INTO int16_numeric_test (num_val) VALUES
    ('100'),
    ('-200'),
    ('99999999999999999999999999999999');

SELECT id, num_val::int16 FROM int16_numeric_test WHERE num_val IS NOT NULL ORDER BY id;

-- Use cast in WHERE clause comparison
SELECT id, big_val FROM int16_numeric_test
WHERE big_val::numeric > '0'::numeric
ORDER BY id;

-- Use cast in arithmetic
SELECT '100'::int16::numeric + '50'::numeric AS sum_result;
SELECT '100'::numeric::int16 + '50'::int16 AS sum_int16;

-- Drop the test table
DROP TABLE int16_numeric_test;

--
-- Comparison of cast results with expected values
--

-- int16 values cast to numeric should compare equal
SELECT '170141183460469231731687303715884105727'::int16::numeric =
       '170141183460469231731687303715884105727'::numeric AS max_equal;

SELECT '-170141183460469231731687303715884105728'::int16::numeric =
       '-170141183460469231731687303715884105728'::numeric AS min_equal;

-- Numeric cast to int16 should compare equal
SELECT '123456789012345678901234567890'::numeric::int16 =
       '123456789012345678901234567890'::int16 AS roundtrip_equal;
