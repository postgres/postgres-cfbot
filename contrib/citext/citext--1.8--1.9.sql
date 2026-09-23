/* contrib/citext/citext--1.8--1.9.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION citext UPDATE TO '1.9'" to load this file. \quit

-- Follow the core split_part(): a negative position counts from the end, a
-- position past the last field returns an empty string, and a position of
-- zero is an error.  A zero position and an empty delimiter, where there is no
-- case to ignore, are left to the core function.
CREATE OR REPLACE FUNCTION split_part( citext, citext, int ) RETURNS TEXT
LANGUAGE SQL IMMUTABLE STRICT PARALLEL SAFE
RETURN CASE
  WHEN $3 = 0 OR pg_catalog.length($2::pg_catalog.text) = 0 THEN
    pg_catalog.split_part($1::pg_catalog.text, $2::pg_catalog.text, $3)
  ELSE
    (SELECT COALESCE(fields[CASE WHEN $3 > 0 THEN $3
                                 ELSE pg_catalog.array_length(fields, 1) + $3 + 1 END],
                     '')
     FROM (SELECT pg_catalog.regexp_split_to_array( $1::pg_catalog.text, pg_catalog.regexp_replace($2::pg_catalog.text, '([^a-zA-Z_0-9])', E'\\\\\\1', 'g'), 'i') AS fields) AS s)
END;
