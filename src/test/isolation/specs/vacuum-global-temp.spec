# Test vacuuming global temporary relations

teardown {
  DROP FUNCTION save_xids, cmp_xids, check_new_xids;
  DROP TABLE vactest, saved_xids;
}

session s1
step create {
  CREATE GLOBAL TEMP TABLE vactest (a int);
  CREATE TABLE saved_xids(local_xid bigint, global_xid bigint, db_xid bigint);

  CREATE FUNCTION save_xids() RETURNS void
  BEGIN ATOMIC
    DELETE FROM saved_xids;
    INSERT INTO saved_xids VALUES (
      (SELECT min(relfrozenxid::text::bigint) FROM pg_temp_class WHERE relfrozenxid != 0),
      (SELECT min(relfrozenxid::text::bigint) FROM pg_class WHERE relfrozenxid != 0),
      (SELECT datfrozenxid::text::bigint FROM pg_database WHERE datname = current_database())
    );
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
                                 OUT db_xid text)
  BEGIN ATOMIC
    WITH new_xids(new_local_xid, new_global_xid, new_db_xid) AS (
      SELECT
        (SELECT min(relfrozenxid::text::bigint) FROM pg_temp_class WHERE relfrozenxid != 0),
        (SELECT min(relfrozenxid::text::bigint) FROM pg_class WHERE relfrozenxid != 0),
        (SELECT datfrozenxid::text::bigint FROM pg_database WHERE datname = current_database())
    )
    SELECT cmp_xids(new_local_xid, local_xid),
           cmp_xids(new_global_xid, global_xid),
           cmp_xids(new_db_xid, db_xid)
    FROM saved_xids, new_xids;
  END;
}
step vacdml1 {
  INSERT INTO vactest SELECT * FROM generate_series(1, 10);
  DELETE FROM vactest WHERE a % 2 = 0;
  INSERT INTO vactest SELECT a * 2 FROM vactest;
}
step vac1prep { SELECT save_xids(); }
step vac1 { VACUUM (FREEZE); }
step vac1cmp { SELECT * FROM check_new_xids(); }

session s2
step vacdml2 {
  INSERT INTO vactest SELECT * FROM generate_series(1, 10);
  UPDATE vactest SET a = a * 2 WHERE a % 2 = 1;
}
step vac2prep { SELECT save_xids(); }
step vac2 { VACUUM (FREEZE); }
step vac2cmp { SELECT * FROM check_new_xids(); }

permutation
  create vacdml1 vac1 vacdml2 vac2
  vacdml1 vac1prep vac1 vac1cmp
  vacdml2 vac2prep vac2 vac2cmp
  vacdml1 vac1prep vac1 vac1cmp
  vacdml2 vac2prep vac2 vac2cmp
