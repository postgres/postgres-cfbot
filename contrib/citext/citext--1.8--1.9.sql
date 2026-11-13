/* contrib/citext/citext--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.9'" to load this file. \quit

-- Normalize the case of the delimiter occurrences in the string, and let the
-- core split_part() do the splitting.  The fields themselves are not touched,
-- so they keep their original case, while the field positions, the empty
-- delimiter and the zero-position error all behave exactly as in core.
-- The delimiter is escaped twice: once as a regular expression to search for,
-- and once as a replacement string, where a backslash is also special.
CREATE OR REPLACE FUNCTION split_part( citext, citext, int ) RETURNS TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN pg_catalog.split_part(
         pg_catalog.regexp_replace(
           $1::pg_catalog.text,
           pg_catalog.regexp_replace($2::pg_catalog.text,
                                     '([^a-zA-Z_0-9])', E'\\\\\\1', 'g'),
           pg_catalog.replace($2::pg_catalog.text, E'\\', E'\\\\'),
           'gi'),
         $2::pg_catalog.text,
         $3);
