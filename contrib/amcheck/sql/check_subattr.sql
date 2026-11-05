--
-- Updates of parts of jsonb, array and composite columns
--
-- The executor works out from an UPDATE's SET expressions which parts of a
-- column it can change, and skips evaluating the expression and partial
-- indexes that read only other parts.  A wrong conclusion would leave an
-- index without an entry for a row, so each case checks every index against
-- the heap; assert-enabled builds also evaluate the indexes anyway and fail
-- if the conclusion was wrong.  HOT counts show which updates avoided new
-- index entries, whether or not anything was skipped.
--
SET client_min_messages = warning;
CREATE TABLE subattr_results (name text, hot bigint, upd bigint, amcheck text);
CREATE FUNCTION subattr_probe(name text, upd text, arg jsonb DEFAULT NULL)
RETURNS void LANGUAGE plpgsql AS $f$
DECLARE h0 bigint; u0 bigint; ac text := 'ok'; i regclass;
BEGIN
  CREATE TABLE subattr (id int PRIMARY KEY, d jsonb, a int[], x int)
    WITH (fillfactor = 10);
  CREATE INDEX subattr_name ON subattr ((d->>'name'));
  CREATE INDEX subattr_nested ON subattr ((d->'o'->>'k'));
  CREATE INDEX subattr_path ON subattr ((d #>> '{p,q}'));
  CREATE INDEX subattr_arr0 ON subattr ((d->'tags'->>0));
  CREATE INDEX subattr_sub ON subattr ((d['s']));
  CREATE INDEX subattr_part ON subattr (id) WHERE (d->>'flag') = 'on';
  CREATE INDEX subattr_a2 ON subattr ((a[2]));
  INSERT INTO subattr SELECT g,
    jsonb_build_object('name', 'n' || g, 'age', g, 'tags', '[1,2,3]'::jsonb,
                       'o', jsonb_build_object('k', 'k' || g, 'z', 1),
                       'p', jsonb_build_object('q', 'q' || g), 's', 's' || g,
                       'flag', CASE WHEN g % 2 = 0 THEN 'on' ELSE 'off' END,
                       '1', 'one', 'arr', '[10,20,30]'::jsonb),
    ARRAY[g, g + 1, g + 2], 0
    FROM generate_series(1, 50) g;
  IF name LIKE 'trigger%' THEN
    CREATE FUNCTION subattr_trg() RETURNS trigger LANGUAGE plpgsql AS
      $t$BEGIN NEW.d := jsonb_set(NEW.d, '{name}', '"renamed"'); RETURN NEW; END$t$;
    CREATE TRIGGER subattr_trg BEFORE UPDATE ON subattr
      FOR EACH ROW EXECUTE FUNCTION subattr_trg();
  END IF;
  h0 := pg_stat_get_xact_tuples_hot_updated('subattr'::regclass);
  u0 := pg_stat_get_xact_tuples_updated('subattr'::regclass);
  IF arg IS NULL THEN EXECUTE upd; ELSE EXECUTE upd USING arg; END IF;
  FOR i IN SELECT indexrelid::regclass FROM pg_index
           WHERE indrelid = 'subattr'::regclass ORDER BY 1 LOOP
    BEGIN PERFORM bt_index_check(i, true);
    EXCEPTION WHEN OTHERS THEN
      ac := CASE WHEN ac = 'ok' THEN '' ELSE ac || '; ' END ||
            i::text || ': ' || SQLERRM;
    END;
  END LOOP;
  INSERT INTO subattr_results VALUES (name,
    pg_stat_get_xact_tuples_hot_updated('subattr'::regclass) - h0,
    pg_stat_get_xact_tuples_updated('subattr'::regclass) - u0, ac);
  DROP TABLE subattr;
  DROP FUNCTION IF EXISTS subattr_trg();
END $f$;

-- Nothing indexed changes.
SELECT subattr_probe('set-age',            $$UPDATE subattr SET d = jsonb_set(d, '{age}', '1')$$);
SELECT subattr_probe('set-o-z',            $$UPDATE subattr SET d = jsonb_set(d, '{o,z}', '2')$$);
SELECT subattr_probe('set-p-other',        $$UPDATE subattr SET d = jsonb_set(d, '{p,r}', '2')$$);
SELECT subattr_probe('delete-age',         $$UPDATE subattr SET d = d - 'age'$$);
SELECT subattr_probe('delete-keys',        $$UPDATE subattr SET d = d - '{age,zz}'::text[]$$);
SELECT subattr_probe('delete-path-age',    $$UPDATE subattr SET d = d #- '{age}'$$);
SELECT subattr_probe('delete-arr-elem',    $$UPDATE subattr SET d = jsonb_set(d, '{tags}', (d->'tags') - 0)$$);
SELECT subattr_probe('concat-other',       $$UPDATE subattr SET d = d || '{"age": 5, "new": 1}'$$);
SELECT subattr_probe('subscript-age',      $$UPDATE subattr SET d['age'] = '7'$$);
SELECT subattr_probe('nested-mutators',    $$UPDATE subattr SET d = jsonb_set(jsonb_set(d, '{age}', '1'), '{new}', '2')$$);
SELECT subattr_probe('set-lax-age',        $$UPDATE subattr SET d = jsonb_set_lax(d, '{age}', NULL, true, 'delete_key')$$);
SELECT subattr_probe('insert-o-new',       $$UPDATE subattr SET d = jsonb_insert(d, '{o,new}', '1')$$);
SELECT subattr_probe('array-a3',           $$UPDATE subattr SET a[3] = 99$$);
SELECT subattr_probe('array-a9-extend',    $$UPDATE subattr SET a[9] = 99$$);
SELECT subattr_probe('set-arr-elem',       $$UPDATE subattr SET d = jsonb_set(d, '{arr,1}', '5')$$);
SELECT subattr_probe('param-path',         $$UPDATE subattr SET d = jsonb_set(d, ARRAY['age'], $1)$$, '3');
SELECT subattr_probe('coalesce-wrap',      $$UPDATE subattr SET d = COALESCE(jsonb_set(d, '{age}', '1'), d)$$);
SELECT subattr_probe('flag-unchanged',     $$UPDATE subattr SET d = jsonb_set(d, '{age}', '1') WHERE id % 2 = 0$$);

-- An indexed part changes, or might.
SELECT subattr_probe('set-name',           $$UPDATE subattr SET d = jsonb_set(d, '{name}', '"x"')$$);
SELECT subattr_probe('set-o',              $$UPDATE subattr SET d = jsonb_set(d, '{o}', '{}')$$);
SELECT subattr_probe('set-o-k',            $$UPDATE subattr SET d = jsonb_set(d, '{o,k}', '"x"')$$);
SELECT subattr_probe('set-p',              $$UPDATE subattr SET d = jsonb_set(d, '{p}', '1')$$);
SELECT subattr_probe('delete-name',        $$UPDATE subattr SET d = d - 'name'$$);
SELECT subattr_probe('delete-keys-name',   $$UPDATE subattr SET d = d - '{age,name}'::text[]$$);
SELECT subattr_probe('delete-path-name',   $$UPDATE subattr SET d = d #- '{name}'$$);
SELECT subattr_probe('delete-path-o-k',    $$UPDATE subattr SET d = d #- '{o,k}'$$);
SELECT subattr_probe('insert-tags-0',      $$UPDATE subattr SET d = jsonb_insert(d, '{tags,0}', '"x"')$$);
SELECT subattr_probe('insert-tags-1',      $$UPDATE subattr SET d = jsonb_insert(d, '{tags,1}', '"x"')$$);
SELECT subattr_probe('insert-tags-neg',    $$UPDATE subattr SET d = jsonb_insert(d, '{tags,-1}', '"x"')$$);
SELECT subattr_probe('set-tags-0',         $$UPDATE subattr SET d = jsonb_set(d, '{tags,0}', '9')$$);
SELECT subattr_probe('set-tags-neg3',      $$UPDATE subattr SET d = jsonb_set(d, '{tags,-3}', '9')$$);
SELECT subattr_probe('set-tags-sp0',       $$UPDATE subattr SET d = jsonb_set(d, ARRAY['tags',' 0'], '9')$$);
SELECT subattr_probe('set-tags-plus0',     $$UPDATE subattr SET d = jsonb_set(d, ARRAY['tags','+0'], '9')$$);
SELECT subattr_probe('set-tags-00',        $$UPDATE subattr SET d = jsonb_set(d, ARRAY['tags','00'], '9')$$);
SELECT subattr_probe('delete-path-tags-0', $$UPDATE subattr SET d = d #- '{tags,0}'$$);
SELECT subattr_probe('set-lax-tags-0',     $$UPDATE subattr SET d = jsonb_set_lax(d, '{tags,0}', NULL, true, 'delete_key')$$);
SELECT subattr_probe('concat-name',        $$UPDATE subattr SET d = d || '{"name": "x"}'$$);
SELECT subattr_probe('concat-array',       $$UPDATE subattr SET d = d || '[1]'$$);
SELECT subattr_probe('concat-left',        $$UPDATE subattr SET d = '{"name": "x"}' || d$$);
SELECT subattr_probe('whole-replace',      $$UPDATE subattr SET d = '{"name": "x"}'$$);
SELECT subattr_probe('mutator-other-col',  $$UPDATE subattr SET d = jsonb_set(jsonb_build_object('name', 'z' || x), '{age}', '1')$$);
SELECT subattr_probe('case-wrap',          $$UPDATE subattr SET d = CASE WHEN id > 0 THEN jsonb_set(d, '{name}', '"x"') ELSE d END$$);
SELECT subattr_probe('set-lax-name',       $$UPDATE subattr SET d = jsonb_set_lax(d, '{name}', NULL, true, 'delete_key')$$);
SELECT subattr_probe('subscript-name',     $$UPDATE subattr SET d['name'] = '"x"'$$);
SELECT subattr_probe('subscript-s',        $$UPDATE subattr SET d['s'] = '"x"'$$);
SELECT subattr_probe('subscript-int',      $$UPDATE subattr SET d['tags'][0] = '9'$$);
SELECT subattr_probe('set-flag',           $$UPDATE subattr SET d = jsonb_set(d, '{flag}', '"on"')$$);
SELECT subattr_probe('set-1-key',          $$UPDATE subattr SET d = jsonb_set(d, '{1}', '"two"')$$);
SELECT subattr_probe('trigger-renames',    $$UPDATE subattr SET d = jsonb_set(d, '{age}', '1')$$);
SELECT subattr_probe('array-a2',           $$UPDATE subattr SET a[2] = 99$$);
SELECT subattr_probe('array-a-neg',        $$UPDATE subattr SET a[-5] = 99$$);
SELECT subattr_probe('array-whole',        $$UPDATE subattr SET a = a || 99$$);
SELECT subattr_probe('param-path-name',    $$UPDATE subattr SET d = jsonb_set(d, ARRAY['name'], $1)$$, '"x"');
SELECT subattr_probe('param-doc',          $$UPDATE subattr SET d = $1$$, '{"name":"x"}');
SELECT subattr_probe('self-ref-value',     $$UPDATE subattr SET d = jsonb_set(d, '{age}', d->'name')$$);
SELECT subattr_probe('null-doc',           $$UPDATE subattr SET d = jsonb_set(NULL::jsonb, '{age}', '1')$$);

SELECT name, upd, hot, amcheck FROM subattr_results ORDER BY name COLLATE "C";
DROP TABLE subattr_results;
DROP FUNCTION subattr_probe;

-- Shapes that need their own tables: composite fields, domains, generic
-- plans, a strict modifier that returns null, and references the analysis
-- does not follow.  t/007_subattr_proof.pl checks what is concluded.
CREATE TYPE subattr_pair AS (k int, v int);
CREATE DOMAIN subattr_doc AS jsonb CHECK (VALUE ? 'name');
CREATE TABLE subattr (id int PRIMARY KEY, d subattr_doc, p subattr_pair,
                      r jsonb, n jsonb) WITH (fillfactor = 50);
CREATE INDEX subattr_name ON subattr ((d->>'name'));
CREATE INDEX subattr_pk ON subattr (((p).k));
CREATE INDEX subattr_whole ON subattr ((r->>'name')) WHERE r IS NOT NULL;
CREATE INDEX subattr_row ON subattr (id) WHERE subattr IS NOT NULL;
CREATE INDEX subattr_text ON subattr ((n::text));
CREATE INDEX subattr_ep ON subattr ((jsonb_extract_path(n, 'a', 'b')));
INSERT INTO subattr SELECT g, jsonb_build_object('name', 'n' || g),
    ROW(g, g)::subattr_pair, jsonb_build_object('name', 'r' || g),
    '{"a": {"b": 1}}'
  FROM generate_series(1, 4) g;
UPDATE subattr SET d = jsonb_set(d, '{age}', '1');
UPDATE subattr SET p.v = 0;
UPDATE subattr SET p.k = (p).k + 10;
UPDATE subattr SET r = jsonb_set(r, '{age}', '1');
UPDATE subattr SET n = jsonb_set(n, '{c}', '1');
UPDATE subattr SET n = jsonb_set(n, '{a,c}', '1');
UPDATE subattr SET d = jsonb_set(d, ARRAY[NULL, 'x'], '1') WHERE false;
UPDATE subattr SET d = jsonb_set(d, '{age}', (SELECT d->'name'));
PREPARE subattr_p(text[], jsonb) AS
  UPDATE subattr SET d = jsonb_set(d, $1, $2);
SET plan_cache_mode = force_generic_plan;
EXECUTE subattr_p('{age}', '2');
EXECUTE subattr_p('{name}', '"x"');
RESET plan_cache_mode;
DEALLOCATE subattr_p;
-- jsonb_set returns null for a null new value; such rows are evaluated
UPDATE subattr SET r = jsonb_set(r, '{age}', NULL) WHERE id <= 2;
SELECT id, d, p, r FROM subattr ORDER BY id;
SELECT bt_index_check(indexrelid, true) FROM pg_index
  WHERE indrelid = 'subattr'::regclass ORDER BY indexrelid::regclass::text;
DROP TABLE subattr;
DROP DOMAIN subattr_doc;
DROP TYPE subattr_pair;
