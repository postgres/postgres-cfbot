# Test global temporary relations

setup {
  CREATE GLOBAL TEMP TABLE tmp (key int PRIMARY KEY, val text, seq serial);

  CREATE GLOBAL TEMP TABLE tmp_parted (key int PRIMARY KEY, val text) PARTITION BY LIST (key);
  CREATE GLOBAL TEMP TABLE tmp_p1 PARTITION OF tmp_parted FOR VALUES IN (1);
  CREATE GLOBAL TEMP TABLE tmp_p2 PARTITION OF tmp_parted FOR VALUES IN ((2), (3));
}

teardown {
  DROP TABLE tmp, tmp_parted;
}

session s1
setup { SET allow_in_place_tablespaces = true; }
step create_tblspace { CREATE TABLESPACE regress_isolation_tablespace LOCATION ''; }
step list_tblspaces { SELECT spcname FROM pg_tablespace ORDER BY 1; }
step drop_tblspace { DROP TABLESPACE regress_isolation_tablespace; }
step b1 { BEGIN; }
step ins1 { INSERT INTO tmp VALUES (1, 's1'); }
step ins1p1 { INSERT INTO tmp_parted VALUES (1, 's1 p1'); }
step ins1p2 { INSERT INTO tmp_parted VALUES (2, 's1 p2'); }
step sel1 { SELECT * FROM tmp; }
step sel1p { SELECT tableoid::regclass, * FROM tmp_parted; }
step create1 { CREATE GLOBAL TEMP TABLE tmp2 (key int, val text, icol int, bcol box); }
step create1dr { CREATE GLOBAL TEMP TABLE tmp2 (key int, val text) ON COMMIT DELETE ROWS; }
step ins1_2 { INSERT INTO tmp2 VALUES (1, 's1'); }
step sel1_2 { SELECT * FROM tmp2; }
step alter1a { ALTER TABLE tmp2 ALTER COLUMN key SET DATA TYPE numeric; }
step alter1b { ALTER TABLE tmp2 ALTER COLUMN val SET NOT NULL; }
step alter1c { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_nn NOT NULL key; }
step alter1d { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_chk CHECK (key > 0); }
step alter1e { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_pk PRIMARY KEY (key); }
step alter1f { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_un UNIQUE (val); }
step alter1g { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_fk FOREIGN KEY (icol) REFERENCES tmp; }
step alter1h { ALTER TABLE tmp2 ADD CONSTRAINT tmp2_ex EXCLUDE USING gist (bcol WITH &&); }
step uniq_idx1 { CREATE UNIQUE INDEX tmp2_un ON tmp2(val); }
step seltype1 { SELECT key, pg_typeof(key), val FROM tmp2; }
step analyze1 { ANALYZE tmp2; }
step cat1 {
  SELECT (SELECT count(*) FROM pg_temp_class WHERE oid >= 12000),
         (SELECT count(*) FROM pg_temp_statistic);
}
step r1 { ROLLBACK; }
step sp1 { SAVEPOINT sp; }
step rsp1 { ROLLBACK TO SAVEPOINT sp; }
step drop1 { DROP TABLE tmp2; }
step c1 { COMMIT; }
step prep1 { PREPARE TRANSACTION 'tx'; }
step cprep1 { COMMIT PREPARED 'tx'; }
step idx1 { CREATE INDEX tmp_val_idx ON tmp(val); }
step sel1_idx {
  SET enable_seqscan = off;
  SET enable_bitmapscan = off;
  EXPLAIN (COSTS OFF)
  SELECT * FROM tmp WHERE val = 's1';
  SELECT * FROM tmp WHERE val = 's1';
}
step t1 { TRUNCATE tmp; }
step alt_tblspace { ALTER TABLE tmp SET TABLESPACE regress_isolation_tablespace; }
step get_tblspace1 {
  SELECT s1.spcname, s2.spcname,
         regexp_replace(pg_relation_filepath('tmp'), '(\d+)', 'NNN', 'g')
    FROM pg_class c
    JOIN pg_tablespace s1 ON s1.oid = c.reltablespace
    LEFT JOIN pg_temp_class t ON t.oid = c.oid
    JOIN pg_tablespace s2 ON s2.oid = t.reltablespace
   WHERE c.relname = 'tmp';
}
step reset_tblspace { ALTER TABLE tmp SET TABLESPACE pg_default; }

session s2
step b2 { BEGIN; }
step ins2 { INSERT INTO tmp VALUES (1, 's2'); }
step ins2p1 { INSERT INTO tmp_parted VALUES (1, 's2 p1'); }
step ins2p2 { INSERT INTO tmp_parted VALUES (2, 's2 p2'); }
step sel2 { SELECT * FROM tmp; }
step sel2p { SELECT tableoid::regclass, * FROM tmp_parted; }
step t2 { TRUNCATE tmp; }
step c2 { COMMIT; }
step r2 { ROLLBACK; }
step sp2 { SAVEPOINT sp; }
step rsp2 { ROLLBACK TO SAVEPOINT sp; }
step ins2_2 { INSERT INTO tmp2 VALUES (1, 's2'); }
step sel2_2 { SELECT * FROM tmp2; }
step seltype2 { SELECT key, pg_typeof(key), val FROM tmp2; }
step drop2 { DROP TABLE tmp2; }
step sel2_idx {
  SET enable_seqscan = off;
  SET enable_bitmapscan = off;
  EXPLAIN (COSTS OFF)
  SELECT * FROM tmp WHERE val = 's2';
  SELECT * FROM tmp WHERE val = 's2';
}
step reidx2 { REINDEX INDEX tmp_val_idx; }
step get_tblspace2 {
  SELECT s1.spcname, s2.spcname,
         regexp_replace(pg_relation_filepath('tmp'), '(\d+)', 'NNN', 'g')
    FROM pg_class c
    JOIN pg_tablespace s1 ON s1.oid = c.reltablespace
    LEFT JOIN pg_temp_class t ON t.oid = c.oid
    LEFT JOIN pg_tablespace s2 ON s2.oid = t.reltablespace
   WHERE c.relname = 'tmp';
}

# Create test tablespace for remaining tests
permutation create_tblspace list_tblspaces

# Basic effects
permutation ins1 ins2 sel1 sel2
permutation ins1p1 ins1p2 ins2p1 ins2p2 sel1p sel2p

# Test rollback of GTT initialization
permutation ins1 b2 ins2 sel1 sel2 c2 sel1 sel2
permutation ins1 b2 ins2 sel1 sel2 r2 sel1 sel2
permutation ins1 b2 ins2 sel1 sel2 sp2 r2 sel1 sel2
permutation ins1 b2 sp2 ins2 sel1 sel2 rsp2 sel1 sel2 r2 sel1 sel2
permutation ins1 b2 ins2 sp2 t2 rsp2 sel1 sel2 r2 sel1 sel2

# Test prevention of ALTER TABLE with rewrite, if in use
permutation create1 ins1_2
            alter1a alter1b alter1c alter1d alter1e alter1f alter1g alter1h
            ins2_2 seltype1 seltype2 drop1
permutation create1 ins1_2 ins2_2
            alter1a alter1b alter1c alter1d alter1e alter1f alter1g alter1h
            uniq_idx1 seltype1 seltype2 drop1

# Test concurrent ON COMMIT DELETE ROWS
permutation create1dr b1 b2 ins1_2 ins2_2 sel1_2 sel2_2 c1 c2 sel1_2 sel2_2 drop1

# Test DROP with ON COMMIT DELETE ROWS
permutation create1dr ins1_2 ins2_2 drop1 create1dr ins1_2 ins2_2 drop1

# Test GTT inval in prepared transaction
permutation create1 drop2 b1 prep1 cprep1
permutation create1 ins1_2 b1 drop2 prep1 cprep1
permutation create1 b1 ins1_2 drop2 prep1

# Test val index
permutation ins1 idx1 sel1_idx ins2 sel2_idx
permutation ins1 ins2 idx1 sel1_idx sel2_idx
permutation ins1 ins2 idx1 sel1_idx sel2_idx reidx2 sel2_idx

# Test local TRUNCATE
permutation ins1 ins2 t2 sel1 sel2 ins2 t1 sel1 sel2 ins1 t2 sel1 sel2

# Test ALTER TABLE ... SET TABLESPACE
permutation ins1 ins2 alt_tblspace get_tblspace1 get_tblspace2 sel1 sel2 reset_tblspace

# Test global temp catalog tidy-up after DROP
permutation create1 ins1_2 analyze1 cat1 drop1 cat1
permutation create1 ins1_2 analyze1 cat1 drop2 cat1
permutation create1 ins1_2 analyze1 cat1 b1 drop2 cat1 r1 cat1
permutation create1 ins1_2 analyze1 b1 cat1 sp1 drop2 cat1 rsp1 cat1 r1 cat1

# Tidy up
permutation drop_tblspace list_tblspaces
