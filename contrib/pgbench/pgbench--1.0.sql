/* contrib/pgbench/pgbench--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgbench" to load this file. \quit

-- PRNG Seeding
CREATE FUNCTION setseed(seed double precision)
RETURNS void
AS 'MODULE_PATHNAME', 'pgbench_setseed_double'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION setseed(seed bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'pgbench_setseed_int64'
LANGUAGE C STRICT VOLATILE PARALLEL UNSAFE;

-- Uniform Random
CREATE FUNCTION random(min bigint, max bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_int64'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Gaussian (Normal) Random
CREATE FUNCTION random_gaussian(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_gaussian_int64'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Exponential Random
CREATE FUNCTION random_exponential(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_exponential_int64'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Zipfian Random
CREATE FUNCTION random_zipfian(min bigint, max bigint, parameter double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_zipfian_int64'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

-- Poisson Random
CREATE FUNCTION random_poisson(center double precision)
RETURNS bigint
AS 'MODULE_PATHNAME', 'pgbench_random_poisson_int64'
LANGUAGE C STRICT VOLATILE PARALLEL SAFE;

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
