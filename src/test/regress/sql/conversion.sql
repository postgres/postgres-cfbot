--
-- create user defined conversion
--

-- directory paths and dlsuffix are passed to us in environment variables
\getenv libdir PG_LIBDIR
\getenv dlsuffix PG_DLSUFFIX

\set regresslib :libdir '/regress' :dlsuffix

CREATE FUNCTION test_enc_setup() RETURNS void
    AS :'regresslib', 'test_enc_setup'
    LANGUAGE C STRICT;
SELECT FROM test_enc_setup();

CREATE FUNCTION test_enc_conversion(bytea, name, name, bool, validlen OUT int, result OUT bytea)
    AS :'regresslib', 'test_enc_conversion'
    LANGUAGE C STRICT;

CREATE USER regress_conversion_user WITH NOCREATEDB NOCREATEROLE;
SET SESSION AUTHORIZATION regress_conversion_user;
CREATE CONVERSION myconv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
--
-- cannot make same name conversion in same schema
--
CREATE CONVERSION myconv FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
--
-- create default conversion with qualified name
--
CREATE DEFAULT CONVERSION public.mydef FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
--
-- cannot make default conversion with same schema/for_encoding/to_encoding
--
CREATE DEFAULT CONVERSION public.mydef2 FOR 'LATIN1' TO 'UTF8' FROM iso8859_1_to_utf8;
-- test comments
COMMENT ON CONVERSION myconv_bad IS 'foo';
COMMENT ON CONVERSION myconv IS 'bar';
COMMENT ON CONVERSION myconv IS NULL;
--
-- drop user defined conversion
--
DROP CONVERSION myconv;
DROP CONVERSION mydef;
--
-- Note: the built-in conversions are exercised in opr_sanity.sql,
-- so there's no need to do that here.
--
--
-- return to the superuser
--
RESET SESSION AUTHORIZATION;
DROP USER regress_conversion_user;

--
-- Test built-in conversion functions.
--

-- Helper function to test a conversion. Uses the test_enc_conversion function
-- that was created in the create_function_0 test.
create or replace function test_conv(
  input IN bytea,
  src_encoding IN text,
  dst_encoding IN text,

  result OUT bytea,
  errorat OUT bytea,
  error OUT text)
language plpgsql as
$$
declare
  validlen int;
begin
  -- First try to perform the conversion with noError = false. If that errors out,
  -- capture the error message, and try again with noError = true. The second call
  -- should succeed and return the position of the error, return that too.
  begin
    select * into validlen, result from test_enc_conversion(input, src_encoding, dst_encoding, false);
    errorat = NULL;
    error := NULL;
  exception when others then
    error := sqlerrm;
    select * into validlen, result from test_enc_conversion(input, src_encoding, dst_encoding, true);
    errorat = substr(input, validlen + 1);
  end;
  return;
end;
$$;


--
-- UTF-8
--
-- The description column must be unique.
CREATE TABLE utf8_verification_inputs (inbytes bytea, description text PRIMARY KEY);
insert into utf8_verification_inputs  values
  ('\x66006f',	'NUL byte'),
  ('\xaf',		'bare continuation'),
  ('\xc5',		'missing second byte in 2-byte char'),
  ('\xc080',	'smallest 2-byte overlong'),
  ('\xc1bf',	'largest 2-byte overlong'),
  ('\xc280',	'next 2-byte after overlongs'),
  ('\xdfbf',	'largest 2-byte'),
  ('\xe9af',	'missing third byte in 3-byte char'),
  ('\xe08080',	'smallest 3-byte overlong'),
  ('\xe09fbf',	'largest 3-byte overlong'),
  ('\xe0a080',	'next 3-byte after overlong'),
  ('\xed9fbf',	'last before surrogates'),
  ('\xeda080',	'smallest surrogate'),
  ('\xedbfbf',	'largest surrogate'),
  ('\xee8080',	'next after surrogates'),
  ('\xefbfbf',	'largest 3-byte'),
  ('\xf1afbf',	'missing fourth byte in 4-byte char'),
  ('\xf0808080',	'smallest 4-byte overlong'),
  ('\xf08fbfbf',	'largest 4-byte overlong'),
  ('\xf0908080',	'next 4-byte after overlong'),
  ('\xf48fbfbf',	'largest 4-byte'),
  ('\xf4908080',	'smallest too large'),
  ('\xfa9a9a8a8a',	'5-byte');

-- Test UTF-8 verification slow path
select description, (test_conv(inbytes, 'utf8', 'utf8')).* from utf8_verification_inputs;

-- Test UTF-8 verification with ASCII padding appended to provide
-- coverage for algorithms that work on multiple bytes at a time.
-- The error message for a sequence starting with a 4-byte lead
-- will contain all 4 bytes if they are present, so various
-- expressions below add 3 ASCII bytes to the end to ensure
-- consistent error messages.
-- The number 64 below needs to be at least the value of STRIDE_LENGTH in wchar.c.

-- Test multibyte verification in fast path
with test_bytes as (
  select
    inbytes,
    description,
    (test_conv(inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from utf8_verification_inputs
), test_padded as (
  select
    description,
    (test_conv(inbytes || repeat('.', 64)::bytea, 'utf8', 'utf8')).error
  from test_bytes
)
select
  description,
  b.error as orig_error,
  p.error as error_after_padding
from test_padded p
join test_bytes b
using (description)
where p.error is distinct from b.error
order by description;

-- Test ASCII verification in fast path where incomplete
-- UTF-8 sequences fall at the end of the preceding chunk.
with test_bytes as (
  select
    inbytes,
    description,
    (test_conv(inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from utf8_verification_inputs
), test_padded as (
  select
    description,
    (test_conv(repeat('.', 64 - length(inbytes))::bytea || inbytes || repeat('.', 64)::bytea, 'utf8', 'utf8')).error
  from test_bytes
)
select
  description,
  b.error as orig_error,
  p.error as error_after_padding
from test_padded p
join test_bytes b
using (description)
where p.error is distinct from b.error
order by description;

-- Test cases where UTF-8 sequences within short text
-- come after the fast path returns.
with test_bytes as (
  select
    inbytes,
    description,
    (test_conv(inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from utf8_verification_inputs
), test_padded as (
  select
    description,
    (test_conv(repeat('.', 64)::bytea || inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from test_bytes
)
select
  description,
  b.error as orig_error,
  p.error as error_after_padding
from test_padded p
join test_bytes b
using (description)
where p.error is distinct from b.error
order by description;

-- Test cases where incomplete UTF-8 sequences fall at the
-- end of the part checked by the fast path.
with test_bytes as (
  select
    inbytes,
    description,
    (test_conv(inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from utf8_verification_inputs
), test_padded as (
  select
    description,
    (test_conv(repeat('.', 64 - length(inbytes))::bytea || inbytes || repeat('.', 3)::bytea, 'utf8', 'utf8')).error
  from test_bytes
)
select
  description,
  b.error as orig_error,
  p.error as error_after_padding
from test_padded p
join test_bytes b
using (description)
where p.error is distinct from b.error
order by description;

CREATE TABLE utf8_inputs (inbytes bytea, description text);
insert into utf8_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\xc3a4c3b6',	'valid, extra latin chars'),
  ('\xd184d0bed0be',	'valid, cyrillic'),
  ('\x666f6fe8b1a1',	'valid, kanji/Chinese'),
  ('\xe382abe3829a',	'valid, two chars that combine to one in EUC_JIS_2004'),
  ('\xe382ab',		'only first half of combined char in EUC_JIS_2004'),
  ('\xe382abe382',	'incomplete combination when converted EUC_JIS_2004'),
  ('\xecbd94eb81bceba6ac', 'valid, Hangul, Korean'),
  ('\x666f6fefa8aa',	'valid, needs mapping function to convert to GB18030'),
  ('\x66e8b1ff6f6f',	'invalid byte sequence'),
  ('\x66006f',		'invalid, NUL byte'),
  ('\x666f6fe8b100',	'invalid, NUL byte'),
  ('\x666f6fe8b1',	'incomplete character at end');

-- Test UTF-8 verification
select description, (test_conv(inbytes, 'utf8', 'utf8')).* from utf8_inputs;
-- Test conversions from UTF-8
select description, inbytes, (test_conv(inbytes, 'utf8', 'euc_jis_2004')).* from utf8_inputs;
select description, inbytes, (test_conv(inbytes, 'utf8', 'latin1')).* from utf8_inputs;
select description, inbytes, (test_conv(inbytes, 'utf8', 'latin2')).* from utf8_inputs;
select description, inbytes, (test_conv(inbytes, 'utf8', 'latin5')).* from utf8_inputs;
select description, inbytes, (test_conv(inbytes, 'utf8', 'koi8r')).* from utf8_inputs;
select description, inbytes, (test_conv(inbytes, 'utf8', 'gb18030')).* from utf8_inputs;

--
-- EUC_JIS_2004
--
CREATE TABLE euc_jis_2004_inputs (inbytes bytea, description text);
insert into euc_jis_2004_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\x666f6fbedd',	'valid'),
  ('\xa5f7',		'valid, translates to two UTF-8 chars '),
  ('\xbeddbe',		'incomplete char '),
  ('\x666f6f00bedd',	'invalid, NUL byte'),
  ('\x666f6fbe00dd',	'invalid, NUL byte'),
  ('\x666f6fbedd00',	'invalid, NUL byte'),
  ('\xbe04',		'invalid byte sequence');

-- Test EUC_JIS_2004 verification
select description, inbytes, (test_conv(inbytes, 'euc_jis_2004', 'euc_jis_2004')).* from euc_jis_2004_inputs;
-- Test conversions from EUC_JIS_2004
select description, inbytes, (test_conv(inbytes, 'euc_jis_2004', 'utf8')).* from euc_jis_2004_inputs;

--
-- SHIFT-JIS-2004
--
CREATE TABLE shiftjis2004_inputs (inbytes bytea, description text);
insert into shiftjis2004_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\x666f6f8fdb',	'valid'),
  ('\x666f6f81c0',	'valid, no translation to UTF-8'),
  ('\x666f6f82f5',	'valid, translates to two UTF-8 chars '),
  ('\x666f6f8fdb8f',	'incomplete char '),
  ('\x666f6f820a',	'incomplete char, followed by newline '),
  ('\x666f6f008fdb',	'invalid, NUL byte'),
  ('\x666f6f8f00db',	'invalid, NUL byte'),
  ('\x666f6f8fdb00',	'invalid, NUL byte');

-- Test SHIFT-JIS-2004 verification
select description, inbytes, (test_conv(inbytes, 'shiftjis2004', 'shiftjis2004')).* from shiftjis2004_inputs;
-- Test conversions from SHIFT-JIS-2004
select description, inbytes, (test_conv(inbytes, 'shiftjis2004', 'utf8')).* from shiftjis2004_inputs;
select description, inbytes, (test_conv(inbytes, 'shiftjis2004', 'euc_jis_2004')).* from shiftjis2004_inputs;

--
-- GB18030
--
CREATE TABLE gb18030_inputs (inbytes bytea, description text);
insert into gb18030_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\x666f6fcff3',	'valid'),
  ('\x666f6f8431a530',	'valid, no translation to UTF-8'),
  ('\x666f6f84309c38',	'valid, translates to UTF-8 by mapping function'),
  ('\xa6d9',  'valid, changed from version 2000 to 2022'),
  ('\x666f6f84309c',	'incomplete char '),
  ('\x666f6f84309c0a',	'incomplete char, followed by newline '),
  ('\x666f6f84',		'incomplete char at end'),
  ('\x666f6f84309c3800', 'invalid, NUL byte'),
  ('\x666f6f84309c0038', 'invalid, NUL byte');

-- Test GB18030 verification.  Round-trip through text so the backing of the
-- bytea values is palloc, not shared_buffers.  This lets Valgrind detect
-- reads past the end.
select description, inbytes, (test_conv(inbytes::text::bytea, 'gb18030', 'gb18030')).* from gb18030_inputs;
-- Test conversions from GB18030
select description, inbytes, (test_conv(inbytes, 'gb18030', 'utf8')).* from gb18030_inputs;


--
-- ISO-8859-5
--
CREATE TABLE iso8859_5_inputs (inbytes bytea, description text);
insert into iso8859_5_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\xe4dede',		'valid'),
  ('\x00',		'invalid, NUL byte'),
  ('\xe400dede',	'invalid, NUL byte'),
  ('\xe4dede00',	'invalid, NUL byte');

-- Test ISO-8859-5 verification
select description, inbytes, (test_conv(inbytes, 'iso8859-5', 'iso8859-5')).* from iso8859_5_inputs;
-- Test conversions from ISO-8859-5
select description, inbytes, (test_conv(inbytes, 'iso8859-5', 'utf8')).* from iso8859_5_inputs;
select description, inbytes, (test_conv(inbytes, 'iso8859-5', 'koi8r')).* from iso8859_5_inputs;

--
-- Big5
--
CREATE TABLE big5_inputs (inbytes bytea, description text);
insert into big5_inputs  values
  ('\x666f6f',		'valid, pure ASCII'),
  ('\x666f6fb648',	'valid'),
  ('\x666f6fa27f',	'valid, no translation to UTF-8'),
  ('\x666f6fb60048',	'invalid, NUL byte'),
  ('\x666f6fb64800',	'invalid, NUL byte');

-- Test Big5 verification
select description, inbytes, (test_conv(inbytes, 'big5', 'big5')).* from big5_inputs;
-- Test conversions from Big5
select description, inbytes, (test_conv(inbytes, 'big5', 'utf8')).* from big5_inputs;

--
-- Direct conversions between single-byte Cyrillic encodings
--
-- The hand-written direct conversion tables must agree with
-- conversion via UTF-8: a direct conversion must not return a
-- different character, must not succeed for a character that has
-- no representation in the target encoding, and must not fail
-- for a character that does.  (Bug #19831)
--

-- KOI8-R 0xa4 is U+2553; in WIN866 that is 0xd6, not 0xf3.
SELECT encode(convert(decode('a4', 'hex'), 'KOI8R', 'WIN866'), 'hex') AS koi8r_a4_to_win866;
SELECT encode(convert(decode('a441a6', 'hex'), 'KOI8R', 'WIN866'), 'hex') AS koi8r_string_to_win866;
-- ISO-8859-5 0xa0 is U+00A0, which WIN1251 has at 0xa0.
SELECT encode(convert(decode('a0', 'hex'), 'ISO_8859_5', 'WIN1251'), 'hex') AS iso8859_5_a0_to_win1251;

-- KOI8R -> WIN1251: every byte that has a WIN1251 representation
SELECT encode(convert(decode('9a9c9ea3b3bfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'KOI8R', 'WIN1251'), 'hex') AS koi8r_to_win1251;
-- KOI8R 0xa4 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('a4', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xa6 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('a6', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xa7 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('a7', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xad has no representation in WIN1251 and must fail
SELECT encode(convert(decode('ad', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xb4 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('b4', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xb6 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('b6', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xb7 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('b7', 'hex'), 'KOI8R', 'WIN1251'), 'hex');
-- KOI8R 0xbd has no representation in WIN1251 and must fail
SELECT encode(convert(decode('bd', 'hex'), 'KOI8R', 'WIN1251'), 'hex');

-- WIN1251 -> KOI8R: every byte that has a KOI8R representation
SELECT encode(convert(decode('a0a8a9b0b7b8c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'WIN1251', 'KOI8R'), 'hex') AS win1251_to_koi8r;
-- WIN1251 0xa5 has no representation in KOI8R and must fail
SELECT encode(convert(decode('a5', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xaa has no representation in KOI8R and must fail
SELECT encode(convert(decode('aa', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xaf has no representation in KOI8R and must fail
SELECT encode(convert(decode('af', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xb2 has no representation in KOI8R and must fail
SELECT encode(convert(decode('b2', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xb3 has no representation in KOI8R and must fail
SELECT encode(convert(decode('b3', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xb4 has no representation in KOI8R and must fail
SELECT encode(convert(decode('b4', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xba has no representation in KOI8R and must fail
SELECT encode(convert(decode('ba', 'hex'), 'WIN1251', 'KOI8R'), 'hex');
-- WIN1251 0xbf has no representation in KOI8R and must fail
SELECT encode(convert(decode('bf', 'hex'), 'WIN1251', 'KOI8R'), 'hex');

-- KOI8R -> WIN866: every byte that has a WIN866 representation
SELECT encode(convert(decode('808182838485868788898a8b8c8d8e8f9091929495969a9c9ea0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbec0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'KOI8R', 'WIN866'), 'hex') AS koi8r_to_win866;

-- WIN866 -> KOI8R: every byte that has a KOI8R representation
SELECT encode(convert(decode('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f8f9fafbfeff', 'hex'), 'WIN866', 'KOI8R'), 'hex') AS win866_to_koi8r;
-- WIN866 0xf2 has no representation in KOI8R and must fail
SELECT encode(convert(decode('f2', 'hex'), 'WIN866', 'KOI8R'), 'hex');
-- WIN866 0xf3 has no representation in KOI8R and must fail
SELECT encode(convert(decode('f3', 'hex'), 'WIN866', 'KOI8R'), 'hex');
-- WIN866 0xf4 has no representation in KOI8R and must fail
SELECT encode(convert(decode('f4', 'hex'), 'WIN866', 'KOI8R'), 'hex');
-- WIN866 0xf5 has no representation in KOI8R and must fail
SELECT encode(convert(decode('f5', 'hex'), 'WIN866', 'KOI8R'), 'hex');

-- WIN866 -> WIN1251: every byte that has a WIN1251 representation
SELECT encode(convert(decode('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8fafcfdff', 'hex'), 'WIN866', 'WIN1251'), 'hex') AS win866_to_win1251;
-- WIN866 0xbd has no representation in WIN1251 and must fail
SELECT encode(convert(decode('bd', 'hex'), 'WIN866', 'WIN1251'), 'hex');
-- WIN866 0xf9 has no representation in WIN1251 and must fail
SELECT encode(convert(decode('f9', 'hex'), 'WIN866', 'WIN1251'), 'hex');

-- WIN1251 -> WIN866: every byte that has a WIN866 representation
SELECT encode(convert(decode('a0a1a2a4a8aaafb0b7b8b9babfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'WIN1251', 'WIN866'), 'hex') AS win1251_to_win866;
-- WIN1251 0xa5 has no representation in WIN866 and must fail
SELECT encode(convert(decode('a5', 'hex'), 'WIN1251', 'WIN866'), 'hex');
-- WIN1251 0xb2 has no representation in WIN866 and must fail
SELECT encode(convert(decode('b2', 'hex'), 'WIN1251', 'WIN866'), 'hex');
-- WIN1251 0xb3 has no representation in WIN866 and must fail
SELECT encode(convert(decode('b3', 'hex'), 'WIN1251', 'WIN866'), 'hex');
-- WIN1251 0xb4 has no representation in WIN866 and must fail
SELECT encode(convert(decode('b4', 'hex'), 'WIN1251', 'WIN866'), 'hex');

-- ISO_8859_5 -> KOI8R: every byte that has a KOI8R representation
SELECT encode(convert(decode('a0a1b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff1', 'hex'), 'ISO_8859_5', 'KOI8R'), 'hex') AS iso_8859_5_to_koi8r;

-- KOI8R -> ISO_8859_5: every byte that has a ISO_8859_5 representation
SELECT encode(convert(decode('9aa3b3c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'KOI8R', 'ISO_8859_5'), 'hex') AS koi8r_to_iso_8859_5;

-- ISO_8859_5 -> WIN1251: every byte that has a WIN1251 representation
SELECT encode(convert(decode('a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'ISO_8859_5', 'WIN1251'), 'hex') AS iso_8859_5_to_win1251;

-- WIN1251 -> ISO_8859_5: every byte that has a ISO_8859_5 representation
SELECT encode(convert(decode('8081838a8c8d8e8f909a9c9d9e9fa0a1a2a3a7a8aaadafb2b3b8b9babcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff', 'hex'), 'WIN1251', 'ISO_8859_5'), 'hex') AS win1251_to_iso_8859_5;

-- ISO_8859_5 -> WIN866: every byte that has a WIN866 representation
SELECT encode(convert(decode('a0a1a4a7aeb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f4f7fe', 'hex'), 'ISO_8859_5', 'WIN866'), 'hex') AS iso_8859_5_to_win866;

-- WIN866 -> ISO_8859_5: every byte that has a ISO_8859_5 representation
SELECT encode(convert(decode('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7fcff', 'hex'), 'WIN866', 'ISO_8859_5'), 'hex') AS win866_to_iso_8859_5;
