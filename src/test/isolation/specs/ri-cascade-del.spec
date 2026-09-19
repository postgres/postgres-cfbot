# Setup for referential integrity crash test
setup
{
    CREATE TABLE crash_reentrancia_tabla_autoreferencial (
        id int PRIMARY KEY,
        nombre text,
        padre_id int REFERENCES crash_reentrancia_tabla_autoreferencial(id) ON DELETE CASCADE
    );

    CREATE TABLE crash_reentrancia_segunda_tabla (
        id    int PRIMARY KEY,
        valor text
    );

    CREATE OR REPLACE FUNCTION crash_reentrancia_before_delete()
    RETURNS trigger AS $$
    DECLARE
        v_valor text;
    BEGIN
        IF OLD.id % 2 = 1 THEN 
            RETURN OLD;
        END IF;

        -- Wait for S2 to finish flooding the invalidation message queue
        IF OLD.id = 2 THEN
            PERFORM pg_advisory_lock(0);
            PERFORM pg_advisory_unlock(0);
        END IF;

        IF OLD.id > 4 THEN
            -- This opens the table and forces processing of pending inval messages
            SELECT valor INTO v_valor FROM crash_reentrancia_segunda_tabla WHERE id = OLD.id;
        END IF;
        
        DELETE FROM crash_reentrancia_tabla_autoreferencial WHERE padre_id = OLD.id;
        RETURN OLD;
    END;
    $$ LANGUAGE plpgsql;

    CREATE TRIGGER trg_crash_reentrancia_before_delete
        BEFORE DELETE ON crash_reentrancia_tabla_autoreferencial
        FOR EACH ROW EXECUTE FUNCTION crash_reentrancia_before_delete();

    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (1, 'A', NULL);
    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (2, 'B', 1);
    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (3, 'C', 2);
    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (4, 'D', 3);
    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (5, 'E', 4);
    INSERT INTO crash_reentrancia_tabla_autoreferencial VALUES (6, 'F', 5);

    INSERT INTO crash_reentrancia_segunda_tabla VALUES 
        (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e'), (6, 'f');
}

teardown
{
    DROP TRIGGER trg_crash_reentrancia_before_delete ON crash_reentrancia_tabla_autoreferencial;
    DROP FUNCTION crash_reentrancia_before_delete CASCADE;
    DROP TABLE crash_reentrancia_tabla_autoreferencial CASCADE;
    DROP TABLE crash_reentrancia_segunda_tabla CASCADE;
}

session s1
step s1_delete { DELETE FROM crash_reentrancia_tabla_autoreferencial WHERE id = 1; }

session s2
step s2_lock { SELECT pg_advisory_lock(0); }
step s2_inval {
  DO $$                                    
  BEGIN
    FOR i IN 1..1000 LOOP
      EXECUTE 'CREATE TEMPORARY TABLE t_temp_inval_(id INTEGER PRIMARY KEY)';
      EXECUTE 'DROP TABLE t_temp_inval_';
    END LOOP;
  END;
  $$;
}
step s2_unlock { SELECT pg_advisory_unlock(0); }

# Execution permutation
# S2 locks -> S1 blocks on S2 -> S2 forces inval queue overflow -> S2 unlocks
# S1 awakens -> S1 forces table_open -> invalidation processed -> segfault!
permutation s2_lock s1_delete s2_inval s2_unlock
