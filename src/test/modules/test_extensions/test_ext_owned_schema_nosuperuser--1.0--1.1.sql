-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "ALTER EXTENSION test_ext_owned_schema_nosuperuser UPDATE TO '1.1'" to load this file. \quit

CREATE FUNCTION owned2() RETURNS text
  LANGUAGE SQL AS $$ SELECT 'owned2'::text $$;
