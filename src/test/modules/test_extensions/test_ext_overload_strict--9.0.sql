/* src/test/modules/test_extensions/test_ext_overload_strict--9.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- As 7.0, unqualified: resolved through search_path by RelnameGetRelid.
INSERT INTO cfg VALUES ('extension');
