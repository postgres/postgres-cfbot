/* src/test/modules/test_extensions/test_ext_overload_strict--3.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_ext_overload_strict" to load this file. \quit

-- The only <<< (varchar, varchar) is a plant; naming it as COMMUTATOR must
-- fail as "does not exist", not collide with it when making a shell.
CREATE OPERATOR @extschema@.>>> (leftarg = varchar, rightarg = varchar,
                                 function = @extschema@.opimpl,
                                 commutator = <<<);
