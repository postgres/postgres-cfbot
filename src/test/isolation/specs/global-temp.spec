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
step b1 { BEGIN; }
step ins1 { INSERT INTO tmp VALUES (1, 's1'); }
step ins1p1 { INSERT INTO tmp_parted VALUES (1, 's1 p1'); }
step ins1p2 { INSERT INTO tmp_parted VALUES (2, 's1 p2'); }
step sel1 { SELECT * FROM tmp; }
step sel1p { SELECT tableoid::regclass, * FROM tmp_parted; }
step create1 { CREATE GLOBAL TEMP TABLE tmp2 (key int, val text, icol int, bcol box); }
step create1dr { CREATE GLOBAL TEMP TABLE tmp2 (key int, val text) ON COMMIT DELETE ROWS; }
step ins1_2 { INSERT INTO tmp2 VALUES (1, 's1'); }
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
step drop1 { DROP TABLE tmp2; }
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
