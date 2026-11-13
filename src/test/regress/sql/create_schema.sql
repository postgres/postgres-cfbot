--
-- CREATE_SCHEMA
--

-- Schema creation with elements.

CREATE ROLE regress_create_schema_role SUPERUSER;

-- Cases where schema creation fails as objects are qualified with a schema
-- that does not match with what's expected.
-- This checks all the object types that include schema qualifications.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE SEQUENCE schema_not_existing.seq;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE schema_not_existing.tab (id int);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE VIEW schema_not_existing.view AS SELECT 1;
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE INDEX ON schema_not_existing.tab (id);
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TRIGGER schema_trig BEFORE INSERT ON schema_not_existing.tab
  EXECUTE FUNCTION schema_trig.no_func();
RESET ROLE;

-- Cases where the schema creation succeeds.
-- The schema created matches the role name.
CREATE SCHEMA AUTHORIZATION regress_create_schema_role
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a different role specification and no schema names.
SET ROLE regress_create_schema_role;
CREATE SCHEMA AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_create_schema_role.tab (id int);
\d regress_create_schema_role.tab
DROP SCHEMA regress_create_schema_role CASCADE;
-- Again, with a schema name and a role specification.
CREATE SCHEMA regress_schema_1 AUTHORIZATION CURRENT_ROLE
  CREATE TABLE regress_schema_1.tab (id int);
\d regress_schema_1.tab
DROP SCHEMA regress_schema_1 CASCADE;
RESET ROLE;

-- Test forward-referencing foreign key clauses.
CREATE SCHEMA regress_schema_fk
    CREATE TABLE regress_schema_fk.t2 (
        b int,
        a int REFERENCES t1 DEFERRABLE INITIALLY DEFERRED NOT ENFORCED
              REFERENCES t3 DEFERRABLE INITIALLY DEFERRED,
        CONSTRAINT fk FOREIGN KEY (a) REFERENCES t1 NOT DEFERRABLE)

    CREATE TABLE regress_schema_fk.t1 (a int PRIMARY KEY)

    CREATE TABLE t3 (a int PRIMARY KEY)

    CREATE TABLE t4 (
        b int,
        a int REFERENCES t5 NOT DEFERRABLE ENFORCED
              REFERENCES t6 DEFERRABLE INITIALLY IMMEDIATE,
        CONSTRAINT fk FOREIGN KEY (a) REFERENCES t6 DEFERRABLE INITIALLY DEFERRED)

    CREATE TABLE t5 (a int, b int, PRIMARY KEY (a))

    CREATE TABLE t6 (a int, b int, PRIMARY KEY (a));

\d regress_schema_fk.t2
\d regress_schema_fk.t4

DROP SCHEMA regress_schema_fk CASCADE;

-- Test that pg_temp does not implicitly shadow objects in CREATE SCHEMA
CREATE TYPE public.regress_temp_shadow_type AS (x int);
CREATE TYPE pg_temp.regress_temp_shadow_type AS (y text);
CREATE TABLE pg_temp.regress_temp_parent (id int PRIMARY KEY);

-- The temp table and the table created in the new schema have the same
-- name, so the FK must resolve to the new schema's table.
CREATE SCHEMA regress_schema_temp
    CREATE TABLE t (a regress_temp_shadow_type)
    CREATE TABLE regress_temp_parent (id int PRIMARY KEY)
    CREATE TABLE child (p_id int REFERENCES regress_temp_parent(id));

SELECT a.attname, t.typname, t.typnamespace::regnamespace
FROM pg_attribute a JOIN pg_type t ON a.atttypid = t.oid
WHERE a.attrelid = 'regress_schema_temp.t'::regclass AND a.attnum > 0;

\d regress_schema_temp.child

-- An explicit pg_temp in the old path is honored
SET search_path = pg_temp, public;

CREATE SCHEMA regress_schema_temp2
    CREATE TABLE t (a regress_temp_shadow_type);

RESET search_path;

SELECT a.attname, t.typname,
       t.typnamespace = pg_my_temp_schema() AS is_temp
FROM pg_attribute a JOIN pg_type t ON a.atttypid = t.oid
WHERE a.attrelid = 'regress_schema_temp2.t'::regclass AND a.attnum > 0;

DROP SCHEMA regress_schema_temp CASCADE;
DROP SCHEMA regress_schema_temp2 CASCADE;
DROP TYPE public.regress_temp_shadow_type CASCADE;

-- Clean up
DROP ROLE regress_create_schema_role;
