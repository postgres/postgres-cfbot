/* contrib/pgtrashcan/pgtrashcan--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgtrashcan" to load this file. \quit

-- ************************************************************************
-- INFRASTRUCTURE SETUP
--
-- The pgtrashcan schema and catalog table are defined here in SQL.
-- This is the source of truth for the schema definition.
--
-- The C code (pgtrashcan.c) only verifies that the schema and catalog table
-- exist (ensure_trash_catalog_exists()). It does NOT recreate them.
-- This file is the single source of truth for the schema definition.
-- For version upgrades, create an upgrade script (e.g., pgtrashcan--1.0--2.0.sql)
-- that uses ALTER TABLE ADD COLUMN etc. The C code uses SPI_fnumber() for
-- column lookups, so missing columns will produce clear errors if the
-- upgrade script hasn't been run.
-- ************************************************************************

-- Create pgtrashcan schema (owned by current user, typically the database owner)
CREATE SCHEMA IF NOT EXISTS pgtrashcan;

-- Create the catalog table that tracks all trashed objects
CREATE TABLE IF NOT EXISTS pgtrashcan._trash_catalog (
    trash_name NAME PRIMARY KEY,
    orig_name NAME NOT NULL,
    orig_schema NAME NOT NULL,
    orig_oid OID NOT NULL,
    orig_obj_type TEXT NOT NULL CHECK (orig_obj_type IN ('table', 'index', 'sequence')),
    drop_time TIMESTAMPTZ NOT NULL DEFAULT now(),
    dropped_by NAME NOT NULL DEFAULT current_user,
    dependent_on_oid OID,
    cascade_parent_oid OID,
    metadata JSONB
);

-- Indexes for fast lookups
CREATE INDEX IF NOT EXISTS idx_trash_catalog_orig_name
    ON pgtrashcan._trash_catalog(orig_name, drop_time DESC);

CREATE INDEX IF NOT EXISTS idx_trash_catalog_orig_schema
    ON pgtrashcan._trash_catalog(orig_schema);

CREATE INDEX IF NOT EXISTS idx_trash_catalog_orig_oid
    ON pgtrashcan._trash_catalog(orig_oid);

CREATE INDEX IF NOT EXISTS idx_trash_catalog_dependent_on
    ON pgtrashcan._trash_catalog(dependent_on_oid) WHERE dependent_on_oid IS NOT NULL;

-- C trigger function to protect catalog from direct user DML
-- Checks pgtrashcan.internal_operation GUC (PGC_SUSET = superuser-only)
CREATE FUNCTION pgtrashcan_protect_catalog()
RETURNS TRIGGER AS 'pgtrashcan'
LANGUAGE C;

CREATE TRIGGER protect_trash_catalog
    BEFORE INSERT OR UPDATE OR DELETE
    ON pgtrashcan._trash_catalog
    FOR EACH ROW
    EXECUTE FUNCTION pgtrashcan_protect_catalog();

-- ************************************************************************
-- FUNCTION DEFINITIONS
-- ************************************************************************

-- Function to list all dropped tables with full metadata
CREATE OR REPLACE FUNCTION pgtrashcan_list_dropped()
RETURNS TABLE(
    trash_name name,
    orig_name name,
    orig_schema name,
    orig_oid oid,
    obj_type text,
    drop_time timestamptz,
    dropped_by name,
    is_dependent boolean,
    cascade_parent_oid oid
)
LANGUAGE SQL
AS $$
    SELECT
        trash_name,
        orig_name,
        orig_schema,
        orig_oid,
        orig_obj_type,
        drop_time,
        dropped_by,
        dependent_on_oid IS NOT NULL AS is_dependent,
        cascade_parent_oid
    FROM pgtrashcan._trash_catalog
    WHERE orig_obj_type = 'table'  -- Only show tables, not dependent objects
    ORDER BY drop_time DESC;
$$;

COMMENT ON FUNCTION pgtrashcan_list_dropped() IS
'Lists all dropped tables currently in the trash with their metadata';

-- Function to list dropped tables grouped by CASCADE drop operations
CREATE OR REPLACE FUNCTION pgtrashcan_list_dropped_grouped()
RETURNS TABLE(
    drop_group int,
    relationship text,
    trash_name name,
    orig_name name,
    orig_schema name,
    orig_oid oid,
    drop_time timestamptz,
    dropped_by name
)
LANGUAGE SQL
AS $$
    WITH RECURSIVE roots AS (
        /*
         * Determine the "root" for each table:
         * - If cascade_parent_oid IS NULL → this table is either standalone or a CASCADE root
         * - If cascade_parent_oid points to a row that still exists → the root is the
         *   top-level ancestor (follow the chain up)
         * - If cascade_parent_oid points to a purged row → orphan, treat as standalone
         *
         * We use a recursive CTE to walk up the cascade_parent_oid chain to find the
         * ultimate root (the table the user explicitly dropped).
         */
        SELECT
            t.orig_oid,
            t.trash_name,
            t.orig_name,
            t.orig_schema,
            t.drop_time,
            t.dropped_by,
            t.cascade_parent_oid,
            CASE
                -- No parent → standalone or root
                WHEN t.cascade_parent_oid IS NULL THEN t.orig_oid
                -- Parent exists in catalog → use parent's orig_oid as group key
                -- (for deeper nesting, we find the ultimate root below)
                WHEN EXISTS (
                    SELECT 1 FROM pgtrashcan._trash_catalog p
                    WHERE p.orig_oid = t.cascade_parent_oid AND p.orig_obj_type = 'table'
                ) THEN NULL  -- will be resolved by recursive CTE
                -- Parent was purged → orphan, treat as standalone
                ELSE t.orig_oid
            END AS immediate_root
        FROM pgtrashcan._trash_catalog t
        WHERE t.orig_obj_type = 'table'
    ),
    -- Recursive CTE to find the ultimate root for each table
    ancestry AS (
        -- Base case: tables that are already roots (no parent, or orphaned)
        SELECT
            orig_oid,
            orig_oid AS root_oid,
            0 AS depth
        FROM roots
        WHERE immediate_root IS NOT NULL

        UNION ALL

        -- Recursive case: follow cascade_parent_oid up the chain
        SELECT
            r.orig_oid,
            CASE
                -- If parent has no parent → parent is the root
                WHEN p.cascade_parent_oid IS NULL THEN p.orig_oid
                -- If parent's parent doesn't exist in catalog → parent is the root (orphan chain)
                WHEN NOT EXISTS (
                    SELECT 1 FROM pgtrashcan._trash_catalog pp
                    WHERE pp.orig_oid = p.cascade_parent_oid AND pp.orig_obj_type = 'table'
                ) THEN p.orig_oid
                -- Otherwise keep going up (handled by next recursion)
                ELSE NULL
            END AS root_oid,
            a.depth + 1
        FROM roots r
        JOIN pgtrashcan._trash_catalog p ON p.orig_oid = r.cascade_parent_oid AND p.orig_obj_type = 'table'
        JOIN ancestry a ON a.orig_oid = r.cascade_parent_oid
        WHERE r.immediate_root IS NULL
          AND a.root_oid IS NOT NULL
          AND a.depth < 50  -- safety limit for pathological chains
    ),
    resolved AS (
        SELECT
            r.orig_oid,
            r.trash_name,
            r.orig_name,
            r.orig_schema,
            r.drop_time,
            r.dropped_by,
            r.cascade_parent_oid,
            COALESCE(a.root_oid, r.orig_oid) AS root_oid
        FROM roots r
        LEFT JOIN ancestry a ON a.orig_oid = r.orig_oid
    ),
    grouped AS (
        SELECT
            DENSE_RANK() OVER (ORDER BY root_oid) AS drop_group,
            CASE
                WHEN cascade_parent_oid IS NULL THEN
                    CASE
                        -- Root with children → 'parent'
                        WHEN EXISTS (
                            SELECT 1 FROM pgtrashcan._trash_catalog c
                            WHERE c.cascade_parent_oid = resolved.orig_oid AND c.orig_obj_type = 'table'
                        ) THEN 'parent'
                        ELSE 'standalone'
                    END
                WHEN NOT EXISTS (
                    SELECT 1 FROM pgtrashcan._trash_catalog p
                    WHERE p.orig_oid = resolved.cascade_parent_oid AND p.orig_obj_type = 'table'
                ) THEN 'orphan'
                ELSE 'child'
            END AS relationship,
            trash_name,
            orig_name,
            orig_schema,
            orig_oid,
            drop_time,
            dropped_by
        FROM resolved
    )
    SELECT
        drop_group::int,
        relationship,
        trash_name,
        orig_name,
        orig_schema,
        orig_oid,
        drop_time,
        dropped_by
    FROM grouped
    ORDER BY drop_group, relationship = 'parent' DESC, relationship = 'standalone' DESC, orig_name;
$$;

COMMENT ON FUNCTION pgtrashcan_list_dropped_grouped() IS
'Lists all dropped tables grouped by CASCADE drop operations. Shows drop_group number (tables dropped together share a group), relationship (parent/child/standalone/orphan), and standard metadata. Orphans are children whose CASCADE parent was purged.';

-- Function to restore a table by its original OID with enhanced conflict detection
CREATE OR REPLACE FUNCTION pgtrashcan_restore_by_oid(p_orig_oid oid)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_trash_name name;
    v_orig_schema name;
    v_orig_name name;
    v_obj_type text;
    v_dependent_on oid;
    v_cascade_parent oid;
    v_drop_time timestamptz;
    v_conflicts text[];
    v_objects_to_restore text[];
    v_result text;
    v_metadata jsonb;  -- to store FK metadata
    r RECORD;
BEGIN
    -- Session-level advisory lock to prevent concurrent restore of same object
    -- This allows same-transaction usage while preventing cross-session conflicts
    IF NOT pg_try_advisory_lock(p_orig_oid::bigint) THEN
        RAISE EXCEPTION 'Another session is currently restoring OID %.', p_orig_oid
            USING HINT = 'Please wait and try again.';
    END IF;

    -- Use exception handling to ensure lock is always released
    BEGIN

    -- Query catalog for main table
    SELECT trash_name, orig_schema, orig_name, orig_obj_type, dependent_on_oid, cascade_parent_oid, drop_time, metadata
    INTO v_trash_name, v_orig_schema, v_orig_name, v_obj_type, v_dependent_on, v_cascade_parent, v_drop_time, v_metadata
    FROM pgtrashcan._trash_catalog
    WHERE orig_oid = p_orig_oid;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Object with OID % not found in trash', p_orig_oid
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    -- Only allow restoring tables directly (not indexes, sequences, etc.)
    IF v_obj_type != 'table' THEN
        RAISE EXCEPTION 'Cannot directly restore object of type "%" (OID: %). Only tables can be restored directly.',
            v_obj_type, p_orig_oid
            USING HINT = format('This %s will be automatically restored when its parent table is restored.', v_obj_type);
    END IF;

    -- Check if it's a dependent object (index, sequence) vs a CASCADE'd table
    IF v_dependent_on IS NOT NULL THEN
        RAISE EXCEPTION 'Cannot directly restore dependent object of type "%" (OID: %). This object belongs to table with OID %.',
            v_obj_type, p_orig_oid, v_dependent_on
            USING HINT = format('This %s will be automatically restored when its parent table is restored.', v_obj_type);
    END IF;

    -- Check if it's a CASCADE'd table (dropped via DROP CASCADE)
    IF v_cascade_parent IS NOT NULL THEN
        RAISE NOTICE 'This table was moved to trash via CASCADE when parent table (OID: %) was dropped.', v_cascade_parent;
        RAISE NOTICE 'Foreign keys to parent were permanently dropped and will not be recreated.';
    END IF;

    -- Build list of all objects that will be restored
    v_objects_to_restore := ARRAY[]::text[];
    v_objects_to_restore := v_objects_to_restore || format('Table: %I.%I (from %I)', v_orig_schema, v_orig_name, v_trash_name);

    -- Add dependent objects to the list
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = p_orig_oid
        ORDER BY orig_obj_type, orig_name
    LOOP
        v_objects_to_restore := v_objects_to_restore ||
            format('%s: %I.%I (from %I)',
                   CASE r.orig_obj_type
                       WHEN 'index' THEN 'Index'
                       WHEN 'sequence' THEN 'Sequence'
                       ELSE initcap(r.orig_obj_type)
                   END,
                   v_orig_schema, r.orig_name, r.trash_name);
    END LOOP;

    -- Comprehensive conflict detection
    v_conflicts := ARRAY[]::text[];

    -- Check if target table name conflicts
    IF EXISTS (
        SELECT 1
        FROM pg_class c
        JOIN pg_namespace n ON c.relnamespace = n.oid
        WHERE n.nspname = v_orig_schema
          AND c.relname = v_orig_name
          AND c.relkind = 'r'
    ) THEN
        v_conflicts := v_conflicts || format('Table: %I.%I already exists', v_orig_schema, v_orig_name);
    END IF;

    -- Check if dependent object names conflict
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = p_orig_oid
    LOOP
        -- Check for index conflicts
        IF r.orig_obj_type = 'index' AND EXISTS (
            SELECT 1
            FROM pg_class c
            JOIN pg_namespace n ON c.relnamespace = n.oid
            WHERE n.nspname = v_orig_schema
              AND c.relname = r.orig_name
              AND c.relkind = 'i'
        ) THEN
            v_conflicts := v_conflicts || format('Index: %I.%I already exists', v_orig_schema, r.orig_name);
        END IF;

        -- Check for sequence conflicts
        IF r.orig_obj_type = 'sequence' AND EXISTS (
            SELECT 1
            FROM pg_class c
            JOIN pg_namespace n ON c.relnamespace = n.oid
            WHERE n.nspname = v_orig_schema
              AND c.relname = r.orig_name
              AND c.relkind = 'S'
        ) THEN
            v_conflicts := v_conflicts || format('Sequence: %I.%I already exists', v_orig_schema, r.orig_name);
        END IF;
    END LOOP;

    -- If conflicts exist, provide detailed report and abort
    IF array_length(v_conflicts, 1) > 0 THEN
        RAISE EXCEPTION E'Cannot restore due to naming conflicts:\n\nObjects to restore:\n  %\n\nConflicting objects in target schema:\n  %\n\nSolutions:\n1. Drop conflicting objects in target schema first\n2. Use pgtrashcan_rename_in_trash(''trash_name'', ''new_name'') to rename conflicting objects in trash before restoring',
            array_to_string(v_objects_to_restore, E'\n  '),
            array_to_string(v_conflicts, E'\n  ')
            USING HINT = 'Use pgtrashcan_rename_in_trash() to rename any conflicting table, index, or sequence in trash';
    END IF;

    /*
     * Note: We rely on PostgreSQL's native locking for protection:
     * - ALTER TABLE SET SCHEMA acquires AccessExclusiveLock on the table
     * - This prevents concurrent operations on the same table
     * - Different tables can be restored in parallel (different OID locks)
     * - No additional schema-level locks needed
     */

    -- Enable internal_operation to allow ALTER TABLE and RENAME on pgtrashcan objects.
    -- These are SECURITY DEFINER operations, not user-initiated changes.
    SET LOCAL pgtrashcan.internal_operation = true;

    -- Move table back to original schema (keeping trash name initially)
    EXECUTE format('ALTER TABLE pgtrashcan.%I SET SCHEMA %I',
                   v_trash_name, v_orig_schema);

    -- Rename table to original name
    IF v_orig_name != v_trash_name THEN
        EXECUTE format('ALTER TABLE %I.%I RENAME TO %I',
                       v_orig_schema, v_trash_name, v_orig_name);
    END IF;

    -- Rename dependent objects back to original names
    -- Only process indexes and sequences (these move automatically with the table)
    -- Do NOT process tables/views that reference this table (user must restore them separately)
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = p_orig_oid
          AND orig_obj_type IN ('index', 'sequence')  -- Only own dependents, not referencing objects
        ORDER BY orig_obj_type, orig_name
    LOOP
        -- These dependent objects (indexes, sequences) are automatically moved with the table
        -- We just need to rename them back to original names
        IF r.orig_name != r.trash_name THEN
            EXECUTE format('ALTER %s %I.%I RENAME TO %I',
                          CASE r.orig_obj_type
                              WHEN 'index' THEN 'INDEX'
                              WHEN 'sequence' THEN 'SEQUENCE'
                              ELSE UPPER(r.orig_obj_type)
                          END,
                          v_orig_schema, r.trash_name, r.orig_name);
        END IF;
    END LOOP;

    -- Delete catalog entries for this table and its own dependents (indexes, sequences)
    -- Do NOT delete entries for other tables/views that were CASCADE'd - they remain in pgtrashcan
    SET LOCAL pgtrashcan.internal_operation = true;
    DELETE FROM pgtrashcan._trash_catalog
    WHERE orig_oid = p_orig_oid
       OR (dependent_on_oid = p_orig_oid AND orig_obj_type IN ('index', 'sequence'));

    v_result := format('Successfully restored table %s.%s (OID: %s, dropped at: %s)',
                      v_orig_schema, v_orig_name, p_orig_oid, v_drop_time);

    -- Show detailed warnings about dropped foreign keys
    IF v_metadata IS NOT NULL AND v_metadata ? 'foreign_keys_dropped' THEN
        DECLARE
            v_fk_array jsonb;
            v_fk_count int;
            v_fk_item jsonb;
            v_fk_details text[];
        BEGIN
            v_fk_array := v_metadata->'foreign_keys_dropped';
            v_fk_count := jsonb_array_length(v_fk_array);

            IF v_fk_count > 0 THEN
                v_fk_details := ARRAY[]::text[];

                -- Build detailed list of dropped FKs
                FOR i IN 0 .. v_fk_count - 1 LOOP
                    v_fk_item := v_fk_array->i;

                    IF (v_fk_item->>'constraint_type') = 'outgoing' THEN
                        v_fk_details := v_fk_details || format(
                            '  - %s: %s',
                            v_fk_item->>'constraint_name',
                            v_fk_item->>'ddl'
                        );
                    ELSE
                        v_fk_details := v_fk_details || format(
                            '  - %s (on %s.%s): %s',
                            v_fk_item->>'constraint_name',
                            v_fk_item->>'source_schema',
                            v_fk_item->>'source_table',
                            v_fk_item->>'ddl'
                        );
                    END IF;
                END LOOP;

                -- Raise detailed notice about dropped FKs
                RAISE NOTICE E'Table restored without % foreign key constraint(s) that were permanently dropped:\n%',
                    v_fk_count,
                    array_to_string(v_fk_details, E'\n');
            END IF;
        EXCEPTION
            WHEN OTHERS THEN
                -- If FK warning fails, don't block restore
                RAISE WARNING 'Could not display FK warning details: %', SQLERRM;
        END;
    END IF;

    -- Show informational notice about triggers preserved with the table
    IF v_metadata IS NOT NULL AND v_metadata ? 'triggers_dropped' THEN
        DECLARE
            v_trigger_array jsonb;
            v_trigger_count int;
            v_trigger_item jsonb;
            v_trigger_details text[];
            v_missing_funcs text[];
            v_func_name text;
        BEGIN
            v_trigger_array := v_metadata->'triggers_dropped';
            v_trigger_count := jsonb_array_length(v_trigger_array);

            IF v_trigger_count > 0 THEN
                v_trigger_details := ARRAY[]::text[];
                v_missing_funcs := ARRAY[]::text[];

                FOR i IN 0 .. v_trigger_count - 1 LOOP
                    v_trigger_item := v_trigger_array->i;
                    v_func_name := v_trigger_item->>'function';

                    v_trigger_details := v_trigger_details || format(
                        '  - %s: %s %s %s trigger calling %s',
                        v_trigger_item->>'trigger_name',
                        v_trigger_item->>'timing',
                        v_trigger_item->>'event',
                        v_trigger_item->>'level',
                        v_func_name
                    );

                    -- Check if trigger function still exists
                    IF v_func_name IS NOT NULL AND NOT EXISTS (
                        SELECT 1 FROM pg_proc WHERE proname = split_part(v_func_name, '(', 1)
                    ) THEN
                        v_missing_funcs := v_missing_funcs || v_func_name;
                    END IF;
                END LOOP;

                RAISE NOTICE E'Table restored with % trigger(s) preserved:\n%',
                    v_trigger_count,
                    array_to_string(v_trigger_details, E'\n');

                IF array_length(v_missing_funcs, 1) > 0 THEN
                    RAISE WARNING E'% trigger function(s) may no longer exist: %\nThese triggers will fail when fired. Drop or recreate them manually.',
                        array_length(v_missing_funcs, 1),
                        array_to_string(v_missing_funcs, ', ');
                END IF;
            END IF;
        EXCEPTION
            WHEN OTHERS THEN
                RAISE WARNING 'Could not display trigger details: %', SQLERRM;
        END;
    END IF;

    -- Show detailed warnings about dropped views
    IF v_metadata IS NOT NULL AND v_metadata ? 'views_dropped' THEN
        DECLARE
            v_view_array jsonb;
            v_view_count int;
            v_view_item jsonb;
            v_view_details text[];
        BEGIN
            v_view_array := v_metadata->'views_dropped';
            v_view_count := jsonb_array_length(v_view_array);

            IF v_view_count > 0 THEN
                v_view_details := ARRAY[]::text[];

                -- Build detailed list of dropped views in replay-friendly order
                -- (collect_views_recursive stores leaves first; reverse so nested
                --  views come after the views they depend on).
                FOR i IN REVERSE (v_view_count - 1) .. 0 LOOP
                    v_view_item := v_view_array->i;

                    v_view_details := v_view_details || format(
                        '  - %s.%s (%s):%s    %s',
                        v_view_item->>'view_schema',
                        v_view_item->>'view_name',
                        v_view_item->>'view_type',
                        E'\n',
                        v_view_item->>'ddl'
                    );
                END LOOP;

                -- Raise detailed notice about dropped views
                RAISE NOTICE E'Table restored without % view(s)/materialized view(s) that were permanently dropped:\n%',
                    v_view_count,
                    array_to_string(v_view_details, E'\n');
            END IF;
        EXCEPTION
            WHEN OTHERS THEN
                -- If view warning fails, don't block restore
                RAISE WARNING 'Could not display view warning details: %', SQLERRM;
        END;
    END IF;

    -- Show detailed warnings about dropped rules
    IF v_metadata IS NOT NULL AND v_metadata ? 'rules_dropped' THEN
        DECLARE
            v_rule_array jsonb;
            v_rule_count int;
            v_rule_item jsonb;
            v_rule_details text[];
        BEGIN
            v_rule_array := v_metadata->'rules_dropped';
            v_rule_count := jsonb_array_length(v_rule_array);

            IF v_rule_count > 0 THEN
                v_rule_details := ARRAY[]::text[];

                FOR i IN 0 .. v_rule_count - 1 LOOP
                    v_rule_item := v_rule_array->i;

                    v_rule_details := v_rule_details || format(
                        '  - %s (%s event):%s    %s',
                        v_rule_item->>'rule_name',
                        v_rule_item->>'event',
                        E'\n',
                        v_rule_item->>'definition'
                    );
                END LOOP;

                RAISE NOTICE E'Table restored without % rule(s) that were permanently dropped:\n%',
                    v_rule_count,
                    array_to_string(v_rule_details, E'\n');
            END IF;
        EXCEPTION
            WHEN OTHERS THEN
                -- If rule warning fails, don't block restore
                RAISE WARNING 'Could not display rule warning details: %', SQLERRM;
        END;
    END IF;

    -- Show detailed warnings about dropped RLS policies
    IF v_metadata IS NOT NULL AND v_metadata ? 'policies_dropped' THEN
        DECLARE
            v_policy_array jsonb;
            v_policy_count int;
            v_policy_item jsonb;
            v_policy_details text[];
        BEGIN
            v_policy_array := v_metadata->'policies_dropped';
            v_policy_count := jsonb_array_length(v_policy_array);

            IF v_policy_count > 0 THEN
                v_policy_details := ARRAY[]::text[];

                FOR i IN 0 .. v_policy_count - 1 LOOP
                    v_policy_item := v_policy_array->i;

                    v_policy_details := v_policy_details || format(
                        '  - %s: FOR %s TO %s %s%s%s',
                        v_policy_item->>'policy_name',
                        v_policy_item->>'command',
                        v_policy_item->>'roles',
                        v_policy_item->>'permissive',
                        CASE WHEN v_policy_item->>'using' != '' THEN E'\n    USING (' || (v_policy_item->>'using') || ')' ELSE '' END,
                        CASE WHEN v_policy_item->>'check' != '' THEN E'\n    WITH CHECK (' || (v_policy_item->>'check') || ')' ELSE '' END
                    );
                END LOOP;

                RAISE NOTICE E'Table restored without % RLS polic%s that were permanently dropped:\n%',
                    v_policy_count,
                    CASE WHEN v_policy_count = 1 THEN 'y' ELSE 'ies' END,
                    array_to_string(v_policy_details, E'\n');
            END IF;
        EXCEPTION
            WHEN OTHERS THEN
                RAISE WARNING 'Could not display policy warning details: %', SQLERRM;
        END;
    END IF;

    -- Release the session-level advisory lock
    PERFORM pg_advisory_unlock(p_orig_oid::bigint);

    RAISE NOTICE '%', v_result;
    RETURN v_result;

    EXCEPTION
        WHEN OTHERS THEN
            -- Always release lock on error
            PERFORM pg_advisory_unlock(p_orig_oid::bigint);
            RAISE;
    END;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_restore_by_oid(oid) IS
'Restores a dropped table by its original OID. Automatically restores dependent objects (indexes, sequences). Use pgtrashcan_rename_in_trash() first if there are naming conflicts.';

-- Function to restore a table by name (with conflict resolution)
CREATE OR REPLACE FUNCTION pgtrashcan_restore(
    p_table_name text,
    p_schema_name text DEFAULT NULL,
    p_oid_hint oid DEFAULT NULL
)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    v_count int;
    v_trash_name name;
    v_orig_schema name;
    v_orig_oid oid;
    v_drop_time timestamptz;
    r RECORD;
BEGIN
    -- Step 1: Find all matching entries (all tables, including dependents)
    SELECT COUNT(*)
    INTO v_count
    FROM pgtrashcan._trash_catalog
    WHERE orig_name = p_table_name::name
      AND orig_obj_type = 'table'
      -- Allow both independent and dependent tables to be restored
      AND (p_schema_name IS NULL OR orig_schema = p_schema_name)
      AND (p_oid_hint IS NULL OR orig_oid = p_oid_hint);

    -- Step 2: No results
    IF v_count = 0 THEN
        RAISE EXCEPTION 'Table "%" not found in trash%',
            p_table_name,
            CASE WHEN p_schema_name IS NOT NULL
                 THEN format(' in schema "%s"', p_schema_name)
                 ELSE ''
            END
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    -- Step 3: Exactly one result - restore it
    IF v_count = 1 THEN
        SELECT trash_name, orig_schema, orig_oid, drop_time
        INTO v_trash_name, v_orig_schema, v_orig_oid, v_drop_time
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name::name
          AND orig_obj_type = 'table'
          AND (p_schema_name IS NULL OR orig_schema = p_schema_name)
          AND (p_oid_hint IS NULL OR orig_oid = p_oid_hint);

        RETURN pgtrashcan_restore_by_oid(v_orig_oid);
    END IF;

    -- Step 4: Multiple results
    -- Check if they're from different schemas
    SELECT COUNT(DISTINCT orig_schema)
    INTO v_count
    FROM pgtrashcan._trash_catalog
    WHERE orig_name = p_table_name::name
      AND orig_obj_type = 'table';

    IF v_count > 1 AND p_schema_name IS NULL THEN
        -- Multiple schemas, user must specify
        RAISE EXCEPTION 'Multiple tables named "%" found in trash from different schemas. Specify schema name.', p_table_name
            USING HINT = format('Available schemas: %s. Use: SELECT pgtrashcan_restore(''%s'', ''<schema_name>'');',
                (SELECT string_agg(DISTINCT orig_schema, ', ' ORDER BY orig_schema)
                 FROM pgtrashcan._trash_catalog
                 WHERE orig_name = p_table_name::name AND orig_obj_type = 'table'),
                p_table_name);
    END IF;

    -- Step 5: Multiple from same schema - restore latest by default unless user specified OID
    IF p_oid_hint IS NULL THEN
        -- Show options and restore latest
        RAISE NOTICE 'Multiple versions of "%" found in trash:', p_table_name;

        FOR r IN
            SELECT orig_oid, orig_schema, drop_time, dropped_by
            FROM pgtrashcan._trash_catalog
            WHERE orig_name = p_table_name::name
              AND orig_obj_type = 'table'
              AND (p_schema_name IS NULL OR orig_schema = p_schema_name)
            ORDER BY drop_time DESC
        LOOP
            RAISE NOTICE '  OID: %, Schema: %, Dropped at: %, By: %',
                r.orig_oid, r.orig_schema, r.drop_time, r.dropped_by;
        END LOOP;

        RAISE NOTICE 'Restoring most recent version. To restore a specific version, use: SELECT pgtrashcan_restore(''%'', ''%'', <oid>);',
            p_table_name, COALESCE(p_schema_name, 'schema_name');

        -- Restore most recent
        SELECT orig_oid
        INTO v_orig_oid
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name::name
          AND orig_obj_type = 'table'
          AND (p_schema_name IS NULL OR orig_schema = p_schema_name)
        ORDER BY drop_time DESC
        LIMIT 1;

        RETURN pgtrashcan_restore_by_oid(v_orig_oid);
    ELSE
        -- User specified OID - restore that one
        RETURN pgtrashcan_restore_by_oid(p_oid_hint);
    END IF;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_restore(text, text, oid) IS
'Restores a dropped table by name. Handles schema conflicts and multiple versions. Use pgtrashcan_rename_in_trash() first if there are naming conflicts.';

-- Function to permanently delete objects from trash by name
CREATE OR REPLACE FUNCTION pgtrashcan_purge(
    p_table_name text,
    p_schema_name text DEFAULT NULL,
    p_oid_hint oid DEFAULT NULL
)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_trash_name name;
    v_orig_schema name;
    v_orig_oid oid;
    v_count int;
    v_result text;
    v_objects_purged text[];
    r RECORD;
BEGIN
    -- Count matching entries
    IF p_oid_hint IS NOT NULL THEN
        -- Direct OID lookup
        SELECT count(*) INTO v_count
        FROM pgtrashcan._trash_catalog
        WHERE orig_oid = p_oid_hint AND orig_obj_type = 'table';
    ELSIF p_schema_name IS NOT NULL THEN
        -- Schema + name lookup
        SELECT count(*) INTO v_count
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name::name
          AND orig_schema = p_schema_name
          AND orig_obj_type = 'table';
    ELSE
        -- Name only lookup
        SELECT count(*) INTO v_count
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name
          AND orig_obj_type = 'table';
    END IF;

    IF v_count = 0 THEN
        RAISE EXCEPTION 'Table "%" not found in trash', p_table_name
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    IF v_count > 1 AND p_oid_hint IS NULL THEN
        RAISE EXCEPTION 'Multiple versions of "%" found in trash. Specify schema or OID.', p_table_name
            USING HINT = 'Use: SELECT * FROM pgtrashcan_list_dropped() WHERE orig_name = ''' || p_table_name || '''; then use pgtrashcan_purge_by_oid(oid) or specify schema.';
    END IF;

    -- Get the entry to purge
    IF p_oid_hint IS NOT NULL THEN
        SELECT trash_name, orig_schema, orig_oid
        INTO v_trash_name, v_orig_schema, v_orig_oid
        FROM pgtrashcan._trash_catalog
        WHERE orig_oid = p_oid_hint AND orig_obj_type = 'table';
    ELSIF p_schema_name IS NOT NULL THEN
        SELECT trash_name, orig_schema, orig_oid
        INTO v_trash_name, v_orig_schema, v_orig_oid
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name::name
          AND orig_schema = p_schema_name
          AND orig_obj_type = 'table';
    ELSE
        -- Single match - use latest
        SELECT trash_name, orig_schema, orig_oid
        INTO v_trash_name, v_orig_schema, v_orig_oid
        FROM pgtrashcan._trash_catalog
        WHERE orig_name = p_table_name::name
          AND orig_obj_type = 'table'
        ORDER BY drop_time DESC
        LIMIT 1;
    END IF;

    -- Build list of objects that will be purged
    v_objects_purged := ARRAY[]::text[];
    v_objects_purged := v_objects_purged || format('Table: %s (from %s.%s)', p_table_name, v_orig_schema, v_trash_name);

    -- Add dependent objects to the list
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = v_orig_oid
        ORDER BY orig_obj_type, orig_name
    LOOP
        v_objects_purged := v_objects_purged ||
            format('%s: %s (from %s)',
                   CASE r.orig_obj_type
                       WHEN 'index' THEN 'Index'
                       WHEN 'sequence' THEN 'Sequence'
                       ELSE initcap(r.orig_obj_type)
                   END,
                   r.orig_name, r.trash_name);
    END LOOP;

    -- Permanently drop the table from pgtrashcan (CASCADE will drop dependent objects)
    EXECUTE format('DROP TABLE IF EXISTS pgtrashcan.%I CASCADE', v_trash_name);

    -- Delete catalog entries (parent + all dependents)
    SET LOCAL pgtrashcan.internal_operation = true;
    DELETE FROM pgtrashcan._trash_catalog
    WHERE orig_oid = v_orig_oid OR dependent_on_oid = v_orig_oid;

    v_result := format('Permanently deleted table "%s" (OID: %s) and %s dependent objects from trash',
                      p_table_name, v_orig_oid, array_length(v_objects_purged, 1) - 1);

    RAISE NOTICE 'Purged objects: %', array_to_string(v_objects_purged, ', ');
    RAISE NOTICE '%', v_result;
    RETURN v_result;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_purge(text, text, oid) IS
'Permanently deletes a table and its dependent objects from trash (cannot be recovered). Parameters: table_name (required), schema_name (optional), oid_hint (optional).';

-- Function to permanently delete objects from trash by OID
CREATE OR REPLACE FUNCTION pgtrashcan_purge_by_oid(p_orig_oid oid)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_trash_name name;
    v_orig_schema name;
    v_orig_name name;
    v_obj_type text;
    v_dependent_on oid;
    v_cascade_parent oid;
    v_drop_time timestamptz;
    v_objects_purged text[];
    v_result text;
    r RECORD;
BEGIN
    -- Query catalog for main table
    SELECT trash_name, orig_schema, orig_name, orig_obj_type, dependent_on_oid, cascade_parent_oid, drop_time
    INTO v_trash_name, v_orig_schema, v_orig_name, v_obj_type, v_dependent_on, v_cascade_parent, v_drop_time
    FROM pgtrashcan._trash_catalog
    WHERE orig_oid = p_orig_oid;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Object with OID % not found in trash', p_orig_oid
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    -- Only allow purging tables directly (not indexes, sequences)
    IF v_obj_type != 'table' THEN
        RAISE EXCEPTION 'Cannot directly purge object of type "%" (OID: %). Only tables can be purged directly.',
            v_obj_type, p_orig_oid
            USING HINT = format('This %s will be automatically purged when its parent table is purged.', v_obj_type);
    END IF;

    -- Check if it's a CASCADE'd table (warn user but allow purging)
    IF v_cascade_parent IS NOT NULL THEN
        RAISE NOTICE 'This table was moved to trash via CASCADE when parent table (OID: %) was dropped.', v_cascade_parent;
    END IF;

    -- Build list of objects that will be purged
    v_objects_purged := ARRAY[]::text[];
    v_objects_purged := v_objects_purged || format('Table: %s.%s (from %s)', v_orig_schema, v_orig_name, v_trash_name);

    -- Add dependent objects to the list
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = p_orig_oid
        ORDER BY orig_obj_type, orig_name
    LOOP
        v_objects_purged := v_objects_purged ||
            format('%s: %s.%s (from %s)',
                   CASE r.orig_obj_type
                       WHEN 'index' THEN 'Index'
                       WHEN 'sequence' THEN 'Sequence'
                       ELSE initcap(r.orig_obj_type)
                   END,
                   v_orig_schema, r.orig_name, r.trash_name);
    END LOOP;

    -- Permanently drop the table from pgtrashcan (CASCADE will drop dependent objects)
    EXECUTE format('DROP TABLE IF EXISTS pgtrashcan.%I CASCADE', v_trash_name);

    -- Delete catalog entries (parent + all dependents)
    SET LOCAL pgtrashcan.internal_operation = true;
    DELETE FROM pgtrashcan._trash_catalog
    WHERE orig_oid = p_orig_oid OR dependent_on_oid = p_orig_oid;

    v_result := format('Permanently deleted table "%s.%s" (OID: %s, dropped at: %s) and %s dependent objects from trash',
                      v_orig_schema, v_orig_name, p_orig_oid, v_drop_time, array_length(v_objects_purged, 1) - 1);

    RAISE NOTICE 'Purged objects: %', array_to_string(v_objects_purged, ', ');
    RAISE NOTICE '%', v_result;
    RETURN v_result;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_purge_by_oid(oid) IS
'Permanently deletes a table and its dependent objects from trash by OID (cannot be recovered).';

-- Enhanced function to purge all objects from trash with detailed reporting
CREATE OR REPLACE FUNCTION pgtrashcan_purge_all()
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_count int;
    v_objects_purged text[];
    r RECORD;
BEGIN
    -- Count tables to purge (both independent and dependent)
    SELECT count(*) INTO v_count
    FROM pgtrashcan._trash_catalog
    WHERE orig_obj_type = 'table';

    IF v_count = 0 THEN
        RETURN 'pgtrashcan schema is empty - no objects to purge';
    END IF;

    -- Build list of all objects that will be purged
    v_objects_purged := ARRAY[]::text[];
    FOR r IN
        SELECT orig_schema, orig_name, trash_name, drop_time, dropped_by
        FROM pgtrashcan._trash_catalog
        WHERE orig_obj_type = 'table'
        ORDER BY drop_time DESC
    LOOP
        v_objects_purged := v_objects_purged ||
            format('Table: %s.%s (dropped %s by %s)', r.orig_schema, r.orig_name, r.drop_time::text, r.dropped_by);
    END LOOP;

    -- Drop all tables in pgtrashcan schema (except catalog)
    FOR r IN
        SELECT tablename
        FROM pg_tables
        WHERE schemaname = 'pgtrashcan'
        AND tablename != '_trash_catalog'
        ORDER BY tablename
    LOOP
        EXECUTE format('DROP TABLE IF EXISTS pgtrashcan.%I CASCADE', r.tablename);
    END LOOP;

    -- Clear catalog (use DELETE instead of TRUNCATE to avoid extension protection)
    SET LOCAL pgtrashcan.internal_operation = true;
    DELETE FROM pgtrashcan._trash_catalog;

    RAISE NOTICE 'Purged all objects: %', array_to_string(v_objects_purged, ', ');
    RAISE NOTICE 'Permanently deleted % table(s) and all their dependent objects from trash', v_count;
    RETURN format('Permanently deleted %s table(s) and all their dependent objects from trash', v_count);
END;
$$;

COMMENT ON FUNCTION pgtrashcan_purge_all() IS
'Permanently deletes ALL objects from trash (cannot be recovered). Use with caution!';

-- Function to change the restore name for objects in trash (for conflict resolution)
-- This only updates orig_name (the name it will be restored as), not the physical trash name.
CREATE OR REPLACE FUNCTION pgtrashcan_rename_in_trash(
    p_trash_name text,
    p_new_restore_name text
)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_orig_obj_type text;
    v_old_orig_name name;
    v_result text;
BEGIN
    -- Find the object in catalog
    SELECT orig_obj_type, orig_name
    INTO v_orig_obj_type, v_old_orig_name
    FROM pgtrashcan._trash_catalog
    WHERE trash_name = p_trash_name;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Object "%" not found in trash catalog', p_trash_name
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    -- Update only the orig_name (the name it will be restored as)
    SET LOCAL pgtrashcan.internal_operation = true;
    UPDATE pgtrashcan._trash_catalog
    SET orig_name = p_new_restore_name
    WHERE trash_name = p_trash_name;

    v_result := format('Updated %s "%s": will now restore as "%s" (was "%s")',
                      v_orig_obj_type, p_trash_name, p_new_restore_name, v_old_orig_name);

    RAISE NOTICE '%', v_result;
    RETURN v_result;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_rename_in_trash(text, text) IS
'Changes the restore name for an object in trash. The physical trash name stays unchanged (unique by OID). Use this to resolve naming conflicts before restore.';

-- Function to move a trashed table to a user-specified schema (instead of original schema)
-- Useful for "archive" use cases where you want the data but not in the original schema.
-- This is the only legitimate way to move a table out of pgtrashcan to a non-original schema.
CREATE OR REPLACE FUNCTION pgtrashcan_move_to_schema(
    p_trash_name text,
    p_target_schema text
)
RETURNS text
LANGUAGE plpgsql
SECURITY DEFINER
SET search_path = pg_catalog, public
AS $$
DECLARE
    v_orig_name name;
    v_orig_schema name;
    v_orig_oid oid;
    v_obj_type text;
    v_result text;
    r RECORD;
BEGIN
    -- Look up by trash_name
    SELECT orig_name, orig_schema, orig_oid, orig_obj_type
    INTO v_orig_name, v_orig_schema, v_orig_oid, v_obj_type
    FROM pgtrashcan._trash_catalog
    WHERE trash_name = p_trash_name;

    IF NOT FOUND THEN
        RAISE EXCEPTION 'Object "%" not found in trash catalog', p_trash_name
            USING HINT = 'Use pgtrashcan_list_dropped() to see available objects.';
    END IF;

    -- Only tables can be moved directly; indexes/sequences move automatically
    IF v_obj_type != 'table' THEN
        RAISE EXCEPTION 'Cannot directly move object of type "%". Only tables can be moved.',
            v_obj_type
            USING HINT = format('This %s is a dependent object and will move with its parent table.', v_obj_type);
    END IF;

    -- Prevent moving a dependent index/sequence parent (should not happen for tables, but guard anyway)
    IF EXISTS (
        SELECT 1 FROM pgtrashcan._trash_catalog
        WHERE trash_name = p_trash_name AND dependent_on_oid IS NOT NULL
    ) THEN
        RAISE EXCEPTION 'Cannot directly move dependent object "%".', p_trash_name
            USING HINT = 'Move the parent table instead.';
    END IF;

    -- Check target schema exists
    IF NOT EXISTS (
        SELECT 1 FROM pg_namespace WHERE nspname = p_target_schema
    ) THEN
        RAISE EXCEPTION 'Target schema "%" does not exist', p_target_schema;
    END IF;

    -- Block moves into pgtrashcan schema (would re-trash it)
    IF p_target_schema = 'pgtrashcan' THEN
        RAISE EXCEPTION 'Cannot move table to pgtrashcan schema. Use pgtrashcan_purge() to delete or pgtrashcan_restore() to restore.';
    END IF;

    -- Check for table naming conflict in target schema
    IF EXISTS (
        SELECT 1 FROM pg_class c
        JOIN pg_namespace n ON c.relnamespace = n.oid
        WHERE n.nspname = p_target_schema
          AND c.relname = v_orig_name
          AND c.relkind = 'r'
    ) THEN
        RAISE EXCEPTION 'Table "%.%" already exists in target schema',
            p_target_schema, v_orig_name
            USING HINT = 'Rename the trashed object first with pgtrashcan_rename_in_trash() to avoid the conflict.';
    END IF;

    -- Check for dependent object naming conflicts in target schema
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = v_orig_oid
          AND orig_obj_type IN ('index', 'sequence')
    LOOP
        IF r.orig_obj_type = 'index' AND EXISTS (
            SELECT 1 FROM pg_class c
            JOIN pg_namespace n ON c.relnamespace = n.oid
            WHERE n.nspname = p_target_schema
              AND c.relname = r.orig_name
              AND c.relkind = 'i'
        ) THEN
            RAISE EXCEPTION 'Index "%.%" already exists in target schema',
                p_target_schema, r.orig_name
                USING HINT = 'Rename the conflicting index in trash first.';
        END IF;

        IF r.orig_obj_type = 'sequence' AND EXISTS (
            SELECT 1 FROM pg_class c
            JOIN pg_namespace n ON c.relnamespace = n.oid
            WHERE n.nspname = p_target_schema
              AND c.relname = r.orig_name
              AND c.relkind = 'S'
        ) THEN
            RAISE EXCEPTION 'Sequence "%.%" already exists in target schema',
                p_target_schema, r.orig_name
                USING HINT = 'Rename the conflicting sequence in trash first.';
        END IF;
    END LOOP;

    -- Enable internal_operation to bypass the pgtrashcan schema ALTER/RENAME protections.
    -- These are SECURITY DEFINER operations, not user-initiated changes.
    SET LOCAL pgtrashcan.internal_operation = true;

    -- Move table from pgtrashcan to target schema (table keeps its trash_$<oid> name initially)
    EXECUTE format('ALTER TABLE pgtrashcan.%I SET SCHEMA %I', p_trash_name, p_target_schema);

    -- Rename from trash_$<oid> back to original name in the target schema
    IF v_orig_name::text != p_trash_name THEN
        EXECUTE format('ALTER TABLE %I.%I RENAME TO %I',
                       p_target_schema, p_trash_name, v_orig_name);
    END IF;

    -- Rename dependent objects (indexes, sequences) back to original names.
    -- These move automatically with the table via SET SCHEMA.
    FOR r IN
        SELECT trash_name, orig_name, orig_obj_type
        FROM pgtrashcan._trash_catalog
        WHERE dependent_on_oid = v_orig_oid
          AND orig_obj_type IN ('index', 'sequence')
        ORDER BY orig_obj_type, orig_name
    LOOP
        IF r.orig_name != r.trash_name THEN
            EXECUTE format('ALTER %s %I.%I RENAME TO %I',
                          CASE r.orig_obj_type
                              WHEN 'index' THEN 'INDEX'
                              WHEN 'sequence' THEN 'SEQUENCE'
                              ELSE UPPER(r.orig_obj_type)
                          END,
                          p_target_schema, r.trash_name, r.orig_name);
        END IF;
    END LOOP;

    -- Clean up catalog entries for this table and its dependent indexes/sequences
    DELETE FROM pgtrashcan._trash_catalog
    WHERE orig_oid = v_orig_oid
       OR (dependent_on_oid = v_orig_oid AND orig_obj_type IN ('index', 'sequence'));

    v_result := format('Moved table "%s" (originally "%s"."%s") to schema "%s" as "%s"',
                       p_trash_name, v_orig_schema, v_orig_name, p_target_schema, v_orig_name);
    RAISE NOTICE '%', v_result;
    RETURN v_result;
END;
$$;

COMMENT ON FUNCTION pgtrashcan_move_to_schema(text, text) IS
'Moves a trashed table to a specified schema (instead of its original schema). The table is restored with its original name. Use this for archive/migration workflows. Catalog entries are cleaned up. This is the only safe way to move a trash object to a non-original schema.';

