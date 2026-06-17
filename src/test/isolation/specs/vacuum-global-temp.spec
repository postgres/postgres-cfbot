# Test vacuuming global temporary relations

setup {
  CREATE VIEW xids AS
    SELECT
      (SELECT min(t.relfrozenxid::text::bigint)
         FROM pg_class c, LATERAL pg_gtr_info(c.oid) t
        WHERE c.relpersistence = 'g'
          AND t.relfrozenxid != 0) AS local_xid,
      (SELECT min(c.relfrozenxid::text::bigint)
         FROM pg_class c
        WHERE c.relfrozenxid != 0) AS global_xid,
      (SELECT datfrozenxid::text::bigint
         FROM pg_database
        WHERE datname = current_database()) AS db_xid,
      (SELECT a.tempfrozenxid::text::bigint
         FROM pg_stat_activity a
        WHERE a.pid = pg_backend_pid()) AS proc_xid;

  CREATE TABLE saved_xids(local_xid bigint,
                          global_xid bigint,
                          db_xid bigint,
                          proc_xid bigint);

  CREATE FUNCTION save_xids() RETURNS void
  BEGIN ATOMIC
    DELETE FROM saved_xids;
    INSERT INTO saved_xids SELECT * FROM xids x;
  END;

  CREATE FUNCTION cmp_xids(xid1 bigint, xid2 bigint) RETURNS text
  BEGIN ATOMIC
    SELECT CASE
             WHEN xid1 < xid2 THEN 'older'
             WHEN xid1 > xid2 THEN 'younger'
             ELSE 'same'
           END;
  END;

  CREATE FUNCTION check_new_xids(OUT local_xid text,
                                 OUT global_xid text,
                                 OUT db_xid text,
                                 OUT proc_xid text)
  BEGIN ATOMIC
    SELECT cmp_xids(x.local_xid, s.local_xid),
           cmp_xids(x.global_xid, s.global_xid),
           cmp_xids(x.db_xid, s.db_xid),
           cmp_xids(x.proc_xid, s.proc_xid)
    FROM saved_xids s, xids x;
  END;

  CREATE VIEW ocd_xids AS
    SELECT t.relfrozenxid::text::bigint AS rel_xid,
           tt.relfrozenxid::text::bigint AS toast_xid
      FROM pg_class c,
           LATERAL pg_gtr_info(c.oid) t,
           LATERAL pg_gtr_info(c.reltoastrelid) tt
     WHERE c.relname = 'ocdtest';

  CREATE TABLE ocd_saved(rel_xid bigint, toast_xid bigint);

  CREATE FUNCTION ocd_save() RETURNS void
  BEGIN ATOMIC
    DELETE FROM ocd_saved;
    INSERT INTO ocd_saved SELECT * FROM ocd_xids;
  END;
}

teardown {
  DROP FUNCTION save_xids, cmp_xids, check_new_xids, ocd_save;
  DROP TABLE saved_xids, ocd_saved;
  DROP VIEW xids, ocd_xids;
}

session s1
step create { CREATE GLOBAL TEMP TABLE vactest (a int); }
step vacdml1 {
  INSERT INTO vactest SELECT * FROM generate_series(1, 10);
  DELETE FROM vactest WHERE a % 2 = 0;
  INSERT INTO vactest SELECT a * 2 FROM vactest;
}
step vac1prep { SELECT save_xids(); }
step vac1 { VACUUM (FREEZE); }
step vac1cmp { SELECT * FROM check_new_xids(); }
step drop { DROP TABLE vactest; }

step ocd_create {
  CREATE GLOBAL TEMP TABLE ocdtest (a int, b text) ON COMMIT DELETE ROWS;
  ALTER TABLE ocdtest ALTER COLUMN b SET STORAGE EXTERNAL;

  CREATE FUNCTION ocd_check(OUT rel_xid text, OUT toast_xid text,
                            OUT rows bigint, OUT proc_min_ok boolean)
  BEGIN ATOMIC
    SELECT cmp_xids(o.rel_xid, s.rel_xid),
           cmp_xids(o.toast_xid, s.toast_xid),
           (SELECT count(*) FROM ocdtest),
           -- the value published in PGPROC must match the in-memory minimum
           x.proc_xid = (SELECT min(t.relfrozenxid::text::bigint)
                           FROM pg_gtrs_in_use() t
                          WHERE t.relfrozenxid != 0)
    FROM ocd_saved s, ocd_xids o, xids x;
  END;
}
step ocd_use { INSERT INTO ocdtest SELECT g, repeat('x', 3000) FROM generate_series(1, 20) g; }
step ocd_save { SELECT ocd_save(); }
step ocd_check { SELECT * FROM ocd_check(); }
step ocd_trunc { TRUNCATE ocdtest; }
step ocd_drop {
  DROP FUNCTION ocd_check;
  DROP TABLE ocdtest;
}

session s2
step vacdml2 {
  INSERT INTO vactest SELECT * FROM generate_series(1, 10);
  UPDATE vactest SET a = a * 2 WHERE a % 2 = 1;
}
step vac2prep { SELECT save_xids(); }
step vac2 { VACUUM (FREEZE); }
step vac2cmp { SELECT * FROM check_new_xids(); }

# Test that VACUUM advances frozen XIDs
permutation
  create vacdml1 vac1 vacdml2 vac2
  vacdml1 vac1prep vac1 vac1cmp
  vacdml2 vac2prep vac2 vac2cmp
  vacdml1 vac1prep vac1 vac1cmp
  vacdml2 vac2prep vac2 vac2cmp drop

# Test that use of a table with ON COMMIT DELETE ROWS advances frozen XIDs
permutation create ocd_create
  ocd_use ocd_save
  vacdml1 vacdml2 vacdml1 vacdml2
  ocd_use ocd_check ocd_use ocd_check
  ocd_save vacdml1 vacdml2 ocd_trunc ocd_check
  ocd_drop drop
