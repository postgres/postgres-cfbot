/* contrib/pgbench/pgbench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgbench" to load this file. \quit

-- PRNG Seeding
CREATE FUNCTION setseed(seed double precision)
RETURNS void
AS 'MODULE_PATHNAME', 'pgbench_setseed_double'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION setseed(seed bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pgbench_setseed_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION setseed(seed integer)
RETURNS void
AS 'MODULE_PATHNAME', 'pgbench_setseed_int32'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Uniform Random
CREATE FUNCTION random(min bigint, max bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Gaussian (Normal) Random
CREATE FUNCTION random_gaussian(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_gaussian_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Exponential Random
CREATE FUNCTION random_exponential(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_exponential_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Zipfian Random
CREATE FUNCTION random_zipfian(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_zipfian_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Poisson Random
CREATE FUNCTION random_poisson(center double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_poisson_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- MurmurHash2 (64-bit)
CREATE FUNCTION hash_murmur2(val bigint, seed bigint DEFAULT 0)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_hash_murmur2_int64'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- FNV-1a Hash (64-bit)
CREATE FUNCTION hash_fnv1a(val bigint, seed bigint DEFAULT 0)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_hash_fnv1a_int64'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Alias for hash_murmur2
CREATE FUNCTION hash(val bigint, seed bigint DEFAULT 0)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_hash_murmur2_int64'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Pseudorandom Permutation
CREATE FUNCTION permute(val bigint, size bigint, seed bigint DEFAULT 0)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_permute_int64'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- TPC-C Non-Uniform Random (NURand)
CREATE FUNCTION nurand(a bigint, min bigint, max bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_nurand_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION nurand(a bigint, min bigint, max bigint, c bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_nurand_int64'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- TPC-C Customer Last Name Syllable Generator
CREATE FUNCTION c_last(val bigint)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_c_last_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Random Alphanumeric and Numeric String Generators
CREATE FUNCTION random_astring(min_len bigint, max_len bigint)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_random_astring_text'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

CREATE FUNCTION random_nstring(min_len bigint, max_len bigint)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_random_nstring_text'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

-- Date / Timestamp Arithmetic and Window Scaling
CREATE FUNCTION date_add(base_date text, offset_val bigint, unit text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_date_add_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION int_to_date(base_date text, val bigint, unit text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_date_add_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION int_to_date(base_date text, val bigint, max_val bigint, interval_str text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_int_to_date_window_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION to_timestamp(str text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_to_timestamp_text'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

-- Printf-style String Formatting
CREATE FUNCTION format_string(fmt text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_format_string_text'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION format_string(fmt text, VARIADIC args "any")
RETURNS text
AS 'MODULE_PATHNAME', 'pgbench_format_string_text'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Dynamic Array Generation
CREATE FUNCTION array_generate(len bigint, val bigint)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'pgbench_array_generate_const'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE FUNCTION array_generate(len bigint, min_val bigint, max_val bigint)
RETURNS bigint[]
AS 'MODULE_PATHNAME', 'pgbench_array_generate_random'
LANGUAGE C STRICT VOLATILE PARALLEL RESTRICTED;

