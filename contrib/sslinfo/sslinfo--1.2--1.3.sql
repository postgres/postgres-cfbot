/* contrib/sslinfo/sslinfo--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION sslinfo UPDATE TO '1.3'" to load this file. \quit

CREATE FUNCTION
ssl_group_info_with_nulls(OUT group_type text, OUT name text
) RETURNS SETOF record
AS 'MODULE_PATHNAME', 'ssl_group_info'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION
ssl_group_info(OUT group_type text, OUT name text
) RETURNS SETOF record AS $$
SELECT * FROM ssl_group_info_with_nulls() WHERE name IS NOT NULL;
$$ LANGUAGE SQL;
