-- Test the int16 (128-bit signed integer) extension

--
-- I/O tests
--
SELECT '0'::int16;
SELECT '1'::int16;
SELECT '-1'::int16;
SELECT '42'::int16;
SELECT '-42'::int16;
SELECT '9223372036854775807'::int16;  -- INT8_MAX
SELECT '-9223372036854775808'::int16; -- INT8_MIN
SELECT '170141183460469231731687303715884105727'::int16;  -- INT16_MAX
SELECT '-170141183460469231731687303715884105728'::int16; -- INT16_MIN
SELECT '  123  '::int16;  -- whitespace
SELECT '+42'::int16;

-- Invalid input
SELECT ''::int16;
SELECT 'abc'::int16;
SELECT '1.5'::int16;
SELECT '1 2'::int16;
SELECT '170141183460469231731687303715884105728'::int16;  -- overflow (MAX+1)
SELECT '-170141183460469231731687303715884105729'::int16; -- overflow (MIN-1)

--
-- Arithmetic: int16 vs int16
--
SELECT '100'::int16 + '50'::int16;
SELECT '100'::int16 - '50'::int16;
SELECT '100'::int16 * '50'::int16;
SELECT '100'::int16 / '50'::int16;
SELECT '100'::int16 % '30'::int16;
SELECT -'42'::int16;
SELECT +'42'::int16;
SELECT @('-42'::int16);

-- Overflow on arithmetic
SELECT '170141183460469231731687303715884105727'::int16 + '1'::int16;
SELECT '-170141183460469231731687303715884105728'::int16 - '1'::int16;
SELECT '85070591730234615865843651857942052864'::int16 * '3'::int16;
SELECT '-170141183460469231731687303715884105728'::int16 / '-1'::int16;

-- Division by zero
SELECT '1'::int16 / '0'::int16;
SELECT '1'::int16 % '0'::int16;

--
-- Comparison: int16 vs int16
--
SELECT '1'::int16 = '1'::int16;
SELECT '1'::int16 = '2'::int16;
SELECT '1'::int16 <> '2'::int16;
SELECT '1'::int16 < '2'::int16;
SELECT '2'::int16 > '1'::int16;
SELECT '1'::int16 <= '1'::int16;
SELECT '1'::int16 >= '1'::int16;
SELECT int16_cmp('5'::int16, '3'::int16);
SELECT int16_cmp('3'::int16, '5'::int16);
SELECT int16_cmp('5'::int16, '5'::int16);

--
-- Bitwise operators
--
SELECT '255'::int16 & '15'::int16;
SELECT '240'::int16 | '15'::int16;
SELECT '255'::int16 # '15'::int16;
SELECT ~'0'::int16;
SELECT '1'::int16 << 4;
SELECT '256'::int16 >> 4;
SELECT '-1'::int16 >> 1;  -- arithmetic shift

--
-- Cross-type arithmetic: int16 op int8
--
SELECT '100'::int16 + '50'::int8;
SELECT '50'::int8 + '100'::int16;
SELECT '100'::int16 - '50'::int8;
SELECT '50'::int8 - '100'::int16;
SELECT '100'::int16 * '50'::int8;
SELECT '50'::int8 * '100'::int16;
SELECT '100'::int16 / '50'::int8;
SELECT '50'::int8 / '100'::int16;

--
-- Cross-type arithmetic: int16 op int4
--
SELECT '100'::int16 + 50;
SELECT 50 + '100'::int16;
SELECT '100'::int16 - 50;
SELECT 50 - '100'::int16;
SELECT '100'::int16 * 50;
SELECT 50 * '100'::int16;
SELECT '100'::int16 / 50;
SELECT 50 / '100'::int16;

--
-- Cross-type arithmetic: int16 op int2
--
SELECT '100'::int16 + 50::int2;
SELECT 50::int2 + '100'::int16;
SELECT '100'::int16 - 50::int2;
SELECT 50::int2 - '100'::int16;
SELECT '100'::int16 * 50::int2;
SELECT 50::int2 * '100'::int16;
SELECT '100'::int16 / 50::int2;
SELECT 50::int2 / '100'::int16;

--
-- Cross-type comparison
--
SELECT '100'::int16 = '100'::int8;
SELECT '100'::int8 = '100'::int16;
SELECT '100'::int16 < '101'::int8;
SELECT '101'::int8 > '100'::int16;
SELECT '100'::int16 = 100;
SELECT 100 = '100'::int16;
SELECT '100'::int16 < 101;
SELECT 101 > '100'::int16;
SELECT '100'::int16 = 100::int2;
SELECT 100::int2 = '100'::int16;

--
-- Casts
--
SELECT '9223372036854775807'::int8::int16;
SELECT '-9223372036854775808'::int8::int16;
SELECT '100'::int16::int8;
SELECT '100'::int4::int16;
SELECT '100'::int16::int4;
SELECT '100'::int2::int16;
SELECT '100'::int16::int2;

-- Cast overflow
SELECT '170141183460469231731687303715884105727'::int16::int8;
SELECT '2147483648'::int16::int4;
SELECT '32768'::int16::int2;

--
-- Aggregates
--
CREATE TABLE int16_test (x int16);
INSERT INTO int16_test VALUES ('1'), ('2'), ('3'), ('4'), ('5'), (NULL);
SELECT sum(x) FROM int16_test;
SELECT avg(x) FROM int16_test;
SELECT min(x) FROM int16_test;
SELECT max(x) FROM int16_test;
SELECT sum(x) FROM int16_test WHERE x IS NOT NULL;

-- Sum overflow
INSERT INTO int16_test VALUES ('170141183460469231731687303715884105727');
SELECT sum(x) FROM int16_test;
-- Clean up the overflow row
DELETE FROM int16_test WHERE x = '170141183460469231731687303715884105727'::int16;

-- Empty table
DELETE FROM int16_test;
SELECT sum(x) FROM int16_test;
SELECT avg(x) FROM int16_test;
SELECT min(x) FROM int16_test;
SELECT max(x) FROM int16_test;

DROP TABLE int16_test;

--
-- B-tree index test
--
CREATE TABLE int16_bt (x int16);
INSERT INTO int16_bt VALUES ('5'), ('3'), ('8'), ('1'), ('9'), ('2'), ('7'), ('4'), ('6');
CREATE INDEX idx_int16_bt ON int16_bt USING btree (x);
SET enable_seqscan = off;
SELECT * FROM int16_bt WHERE x > '4'::int16 ORDER BY x;
SELECT * FROM int16_bt WHERE x <= '3'::int16 ORDER BY x;
SELECT * FROM int16_bt WHERE x = '7'::int16;
RESET enable_seqscan;
DROP TABLE int16_bt;

--
-- Hash index test
--
CREATE TABLE int16_hash (x int16);
INSERT INTO int16_hash VALUES ('5'), ('3'), ('8'), ('1'), ('9');
CREATE INDEX idx_int16_hash ON int16_hash USING hash (x);
SET enable_seqscan = off;
SELECT * FROM int16_hash WHERE x = '8'::int16;
RESET enable_seqscan;
DROP TABLE int16_hash;
