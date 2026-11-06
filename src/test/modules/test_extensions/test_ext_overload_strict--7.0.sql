/* src/test/modules/test_extensions/test_ext_overload_strict--7.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- A planted table reached by its schema-qualified name, as an upgrade script
-- would reach a table it expects from an earlier version.  (CREATE ... IF NOT
-- EXISTS over the plant is already refused on membership grounds, so the
-- reference must be a bare one.)  The INSERT would write the extension's data
-- into the attacker's table; the qualified lookup must refuse it as "does not
-- exist", exactly as the unqualified one does.
INSERT INTO @extschema@.cfg VALUES ('extension');
