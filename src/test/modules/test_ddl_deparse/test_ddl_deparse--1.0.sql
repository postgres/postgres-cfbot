/* src/test/modules/test_ddl_deparse/test_ddl_deparse--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ddl_deparse" to load this file. \quit

CREATE FUNCTION get_command_type(pg_ddl_command)
  RETURNS text IMMUTABLE STRICT PARALLEL SAFE
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION get_command_tag(pg_ddl_command)
  RETURNS text IMMUTABLE STRICT PARALLEL SAFE
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE FUNCTION get_altertable_subcmdinfo(IN cmd pg_ddl_command,
    OUT cmdtype text,
    OUT objdesc text)
  RETURNS SETOF record IMMUTABLE STRICT PARALLEL SAFE
  AS 'MODULE_PATHNAME' LANGUAGE C;

-- Deparse each supported DDL command as it finishes executing.  The event
-- trigger is created here, rather than by the regression scripts, so that
-- pg_regress --load-extension is enough to install it for a whole run.
CREATE FUNCTION capture_deparsed_ddl()
  RETURNS event_trigger
  AS 'MODULE_PATHNAME' LANGUAGE C;

CREATE EVENT TRIGGER capture_deparsed_ddl
  ON ddl_command_end EXECUTE FUNCTION capture_deparsed_ddl();

-- An origin event trigger does not fire while session_replication_role is
-- 'replica', which would silently stop the capture just where a replication
-- apply worker runs; the module must keep deparsing there too.
ALTER EVENT TRIGGER capture_deparsed_ddl ENABLE ALWAYS;
