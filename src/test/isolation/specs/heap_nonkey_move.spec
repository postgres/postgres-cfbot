# Non-key updates that move a row must remain compatible with KEY SHARE.
# Heap and FLUX twins have identical expected output.
setup
{
  CREATE TABLE fi (id int, v int, PRIMARY KEY (id) INCLUDE (v)) USING heap;
  CREATE TABLE fg (id int PRIMARY KEY, v int, pad text) USING heap WITH (fillfactor=100);
  ALTER TABLE fg ALTER COLUMN pad SET STORAGE PLAIN;
  INSERT INTO fi VALUES (1, 1);
  INSERT INTO fg SELECT g, 1, repeat('x', 640) FROM generate_series(1, 40) g;
  CREATE TABLE ci (id int REFERENCES fi) USING heap;
  CREATE TABLE cg (id int REFERENCES fg) USING heap;
}
teardown { DROP TABLE ci, cg, fi, fg; }

session s1
step rr { BEGIN ISOLATION LEVEL REPEATABLE READ; }
step rc { BEGIN ISOLATION LEVEL READ COMMITTED; }
step snap { SELECT v FROM fi WHERE id=1; SELECT v FROM fg WHERE id=1; }
step ki { SELECT id, v FROM fi WHERE id=1 FOR KEY SHARE; }
step kg { SELECT id, v, length(pad) FROM fg WHERE id=1 FOR KEY SHARE; }
step ii { INSERT INTO ci VALUES (1); }
step ig { INSERT INTO cg VALUES (1); }
step si { SELECT id, v FROM fi WHERE id=1 FOR SHARE; }
step sg { SELECT id, v, length(pad) FROM fg WHERE id=1 FOR SHARE; }
step di { DELETE FROM fi WHERE id=1; }
step dg { DELETE FROM fg WHERE id=1; }
step wi { UPDATE fi SET v=v+1 WHERE id=1; }
step wg { UPDATE fg SET v=v+1 WHERE id=1; }
step c1 { COMMIT; }

session s2
step b2 { BEGIN; }
step ui { UPDATE fi SET v=2 WHERE id=1; }
step ug { UPDATE fg SET pad=repeat('y',1920) WHERE id=1; }
step c2 { COMMIT; }
step r2 { ROLLBACK; }
step d2i { DELETE FROM fi WHERE id=1; }
step n2i { UPDATE fi SET v=3 WHERE id=1; }
step n2g { UPDATE fg SET pad=repeat('z',1940) WHERE id=1; }
step k2i { UPDATE fi SET id=2 WHERE id=1; }
step d2g { DELETE FROM fg WHERE id=1; }
step k2g { UPDATE fg SET id=100 WHERE id=1; }

session s3
step b3 { BEGIN; }
step u3 { UPDATE fi SET v=3 WHERE id=1; }
step k3 { UPDATE fi SET id=2 WHERE id=1; }
step z3 { UPDATE fi SET id=1 WHERE id=2; }
step c3 { COMMIT; }
permutation rr snap b2 ui ki c2 c1
permutation rr snap b2 ui ii c2 c1
permutation rr snap b2 ui si c2 c1
permutation rr snap b2 ui di c2 c1
permutation rr snap b2 ui wi c2 c1
permutation rr snap b2 ug kg c2 c1
permutation rr snap b2 ug ig c2 c1
permutation rr snap b2 ug sg c2 c1
permutation rr snap b2 ug dg c2 c1
permutation rr snap b2 ug wg c2 c1
permutation rc snap b2 ui ki c2 c1
permutation rc snap b2 ui ii c2 c1
permutation rc snap b2 ui si c2 c1
permutation rc snap b2 ui di c2 c1
permutation rc snap b2 ui wi c2 c1
permutation rc snap b2 ug kg c2 c1
permutation rc snap b2 ug ig c2 c1
permutation rc snap b2 ug sg c2 c1
permutation rc snap b2 ug dg c2 c1
permutation rc snap b2 ug wg c2 c1
permutation rr snap b2 ui ki c2 d2i c1
permutation rr snap b2 ui ki c2 k2i c1
permutation rr snap b2 ug kg c2 d2g c1
permutation rr snap b2 ug kg c2 k2g c1
permutation rr snap b2 ui ki r2 c1
permutation rr snap b2 ui c2 ki c1
permutation rr snap b2 ug kg r2 c1
permutation rr snap b2 ug c2 kg c1

permutation rr snap b2 ui ki c2 n2i c1
permutation rr snap b2 ug kg c2 n2g c1

permutation rr snap b2 ui c2 b3 k3 c3 ki c1
permutation rc snap b2 ui c2 b3 k3 c3 ki c1
permutation rr snap b2 ui c2 b3 k3 ki c3 c1
permutation rr snap b2 ui c2 b3 u3 ki c3 c1
permutation rr snap b2 ui c2 b3 k3 z3 u3 c3 ki c1
permutation rr snap b2 ui c2 b3 k3 z3 u3 ki c3 c1
