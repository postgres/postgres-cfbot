-- Tests for UPDATE/DELETE FOR PORTION OF

SET datestyle TO ISO, YMD;

-- Works on non-PK columns
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text NOT NULL
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2020-01-01)', 'one');

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = 'one^1';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-01-15' TO '2019-01-20';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- With a table alias with AS

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-01' TO '2019-02-03' AS t
  SET name = 'one^2';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-03' TO '2019-02-04' AS t;

-- With a table alias without AS

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-04' TO '2019-02-05' t
  SET name = 'one^3';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-02-05' TO '2019-02-06' t;

-- UPDATE with FROM

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-03-01' to '2019-03-02'
  SET name = 'one^4'
  FROM (SELECT '[1,2)'::int4range) AS t2(id)
  WHERE for_portion_of_test.id = t2.id;

-- DELETE with USING

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2019-03-02' TO '2019-03-03'
  USING (SELECT '[1,2)'::int4range) AS t2(id)
  WHERE for_portion_of_test.id = t2.id;

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- Works on more than one range
DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid1_at daterange,
  valid2_at daterange,
  name text NOT NULL
);
INSERT INTO for_portion_of_test (id, valid1_at, valid2_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', '[2015-01-01,2025-01-01)', 'one');

UPDATE for_portion_of_test
  FOR PORTION OF valid1_at FROM '2018-01-15' TO NULL
  SET name = 'foo';
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

UPDATE for_portion_of_test
  FOR PORTION OF valid2_at FROM '2018-01-15' TO NULL
  SET name = 'bar';
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid1_at FROM '2018-01-20' TO NULL;
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

DELETE FROM for_portion_of_test
  FOR PORTION OF valid2_at FROM '2018-01-20' TO NULL;
SELECT * FROM for_portion_of_test ORDER BY id, valid1_at, valid2_at;

-- Test with NULLs in the scalar/range key columns.
-- This won't happen if there is a PRIMARY KEY or UNIQUE constraint
-- but FOR PORTION OF shouldn't require that.
DROP TABLE for_portion_of_test;
CREATE UNLOGGED TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', NULL, '1 null'),
  ('[1,2)', '(,)', '1 unbounded'),
  ('[1,2)', 'empty', '1 empty'),
  (NULL, NULL, NULL),
  (NULL, daterange('2018-01-01', '2019-01-01'), 'null key');
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'NULL to NULL';
SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

DROP TABLE for_portion_of_test;

--
-- UPDATE tests
--

CREATE TABLE for_portion_of_test (
  id int4range NOT NULL,
  valid_at daterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-01-05)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three'),
  ('[4,5)', '(,2018-04-01)', 'four'),
  ('[5,6)', '(,)', 'five')
  ;
\set QUIET false

-- Updating with a missing column fails
UPDATE for_portion_of_test
  FOR PORTION OF invalid_at FROM '2018-06-01' TO NULL
  SET name = 'foo'
  WHERE id = '[5,6)';

-- Updating the range fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  SET valid_at = '[1990-01-01,1999-01-01)'
  WHERE id = '[5,6)';

-- The wrong start type fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM 1 TO '2020-01-01'
  SET name = 'nope'
  WHERE id = '[3,4)';

-- The wrong end type fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2000-01-01' TO 4
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with timestamps reversed fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO '2018-01-01'
  SET name = 'three^1'
  WHERE id = '[3,4)';

-- Updating with a subquery fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM (SELECT '2018-01-01') TO '2018-06-01'
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with a column fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM lower(valid_at) TO NULL
  SET name = 'nope'
  WHERE id = '[3,4)';
-- and so does a whole-row reference, which takes a different path
UPDATE for_portion_of_test
  FOR PORTION OF valid_at ((ROW(for_portion_of_test.*)).f2)
  SET name = 'nope'
  WHERE id = '[3,4)';

-- Updating with timestamps equal does nothing
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-04-01'
  SET name = 'three^0'
  WHERE id = '[3,4)';

-- Updating a finite/open portion with a finite/open target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  SET name = 'three^1'
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Updating a finite/open portion with an open/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-03-01'
  SET name = 'three^2'
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Updating an open/finite portion with an open/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-02-01'
  SET name = 'four^1'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating an open/finite portion with a finite/open target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO NULL
  SET name = 'four^2'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating a finite/finite portion with an exact fit
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO '2018-02-01'
  SET name = 'four^3'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Updating an enclosed span
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'two^2'
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

-- Updating an open/open portion with a finite/finite target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-01' TO '2019-01-01'
  SET name = 'five^1'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Updating an enclosed span with separate protruding spans
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO '2020-01-01'
  SET name = 'five^2'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Updating multiple enclosed spans
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  SET name = 'one^2'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (daterange('2018-03-10', '2018-03-15'))
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target, coerced from a string
UPDATE for_portion_of_test
  FOR PORTION OF valid_at ('[2018-03-15,2018-03-17)')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating with a direct target of the wrong range subtype fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (int4range(1, 4))
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of a non-rangetype fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (4)
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of NULL fails
UPDATE for_portion_of_test
  FOR PORTION OF valid_at (NULL)
  SET name = 'one^3'
  WHERE id = '[1,2)';

-- Updating with a direct target of empty does nothing
UPDATE for_portion_of_test
  FOR PORTION OF valid_at ('empty')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Updating the non-range part of the PK:
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-15' TO NULL
  SET id = '[6,7)'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id IN ('[1,2)', '[6,7)') ORDER BY id, valid_at;

-- UPDATE with no WHERE clause
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2030-01-01' TO NULL
  SET name = name || '*';

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;
\set QUIET true

-- Updating with a shift/reduce conflict
-- (requires a tsrange column)
CREATE UNLOGGED TABLE for_portion_of_test2 (
  id int4range,
  valid_at tsrange,
  name text
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', '[2000-01-01,2020-01-01)', 'one');
-- updates [2011-03-01 01:02:00, 2012-01-01) (note 2 minutes)
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM '2011-03-01'::timestamp + INTERVAL '1:02:03' HOUR TO MINUTE
    TO '2012-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';

-- TO is used for the bound but not the INTERVAL:
-- syntax error
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM '2013-03-01'::timestamp + INTERVAL '1:02:03' HOUR
    TO '2014-01-01'
  SET name = 'one^2'
  WHERE id = '[1,2)';

-- adding parens fixes it
-- updates [2015-03-01 01:00:00, 2016-01-01) (no minutes)
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at
    FROM ('2015-03-01'::timestamp + INTERVAL '1:02:03' HOUR)
    TO '2016-01-01'
  SET name = 'one^3'
  WHERE id = '[1,2)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;
DROP TABLE for_portion_of_test2;

-- UPDATE FOR PORTION OF in a CTE:
-- The outer query sees the table how it was before the updates,
-- and with no leftovers yet,
-- but it also sees the new values via the RETURNING clause.
-- (We test RETURNING more directly, without a CTE, below.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
WITH update_apr AS (
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-05-01'
    SET name = 'Apr 2018'
    WHERE id = '[10,11)'
    RETURNING id, valid_at, name
)
SELECT *
  FROM for_portion_of_test AS t, update_apr
  WHERE t.id = update_apr.id;
SELECT * FROM for_portion_of_test WHERE id = '[10,11)' ORDER BY id, valid_at;

-- UPDATE FOR PORTION OF with current_date
-- (We take care not to make the expectation depend on the timestamp.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[99,100)', '[2000-01-01,)', 'foo');
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM current_date TO null
  SET name = 'bar'
  WHERE id = '[99,100)';
SELECT name, lower(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date - 1;
SELECT name, upper(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date + 1;

-- UPDATE FOR PORTION OF with clock_timestamp()
-- fails because the function is volatile:
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM clock_timestamp()::date TO null
  SET name = 'baz'
  WHERE id = '[99,100)';

-- clean up:
DELETE FROM for_portion_of_test WHERE id = '[99,100)';

-- Not visible to UPDATE:
-- Tuples updated/inserted within the CTE are not visible to the main query yet,
-- but neither are old tuples the CTE changed:
-- (This is the same behavior as without FOR PORTION OF.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[11,12)', '[2018-01-01,2020-01-01)', 'eleven');
WITH update_apr AS (
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-05-01'
    SET name = 'Apr 2018'
    WHERE id = '[11,12)'
    RETURNING id, valid_at, name
)
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  AS t
  SET name = 'May 2018'
  FROM update_apr AS j
  WHERE t.id = j.id;
SELECT * FROM for_portion_of_test WHERE id = '[11,12)' ORDER BY id, valid_at;
DELETE FROM for_portion_of_test WHERE id IN ('[10,11)', '[11,12)');

-- UPDATE FOR PORTION OF in a PL/pgSQL function
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
CREATE FUNCTION fpo_update(_id int4range, _target_from date, _target_til date)
RETURNS void LANGUAGE plpgsql AS
$$
BEGIN
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM $2 TO $3
    SET name = concat(_target_from::text, ' to ', _target_til::text)
    WHERE id = $1;
END;
$$;
SELECT fpo_update('[10,11)', '2015-01-01', '2019-01-01');
SELECT * FROM for_portion_of_test WHERE id = '[10,11)';

-- UPDATE FOR PORTION OF in a compiled SQL function
CREATE FUNCTION fpo_update()
RETURNS text
BEGIN ATOMIC
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
    SET name = 'one^1'
    RETURNING name;
END;
\sf+ fpo_update()
CREATE OR REPLACE function fpo_update()
RETURNS text
BEGIN ATOMIC
  UPDATE for_portion_of_test
    FOR PORTION OF valid_at (daterange('2018-01-15', '2020-01-01') * daterange('2019-01-01', '2022-01-01'))
    SET name = 'one^1'
    RETURNING name;
END;
\sf+ fpo_update()
DROP FUNCTION fpo_update();

DROP TABLE for_portion_of_test;

--
-- DELETE tests
--

CREATE TABLE for_portion_of_test (
  id int4range NOT NULL,
  valid_at daterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-01-05)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three'),
  ('[4,5)', '(,2018-04-01)', 'four'),
  ('[5,6)', '(,)', 'five'),
  ('[6,7)', '[2018-01-01,)', 'six'),
  ('[7,8)', '(,2018-04-01)', 'seven'),
  ('[8,9)', '[2018-01-02,2018-02-03)', 'eight'),
  ('[8,9)', '[2018-02-03,2018-03-03)', 'eight'),
  ('[8,9)', '[2018-03-03,2018-04-04)', 'eight')
  ;
\set QUIET false

-- Deleting with a missing column fails
DELETE FROM for_portion_of_test
  FOR PORTION OF invalid_at FROM '2018-06-01' TO NULL
  WHERE id = '[5,6)';

-- The wrong start type fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM 1 TO '2020-01-01'
  WHERE id = '[3,4)';

-- The wrong end type fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2000-01-01' TO 4
  WHERE id = '[3,4)';

-- Deleting with timestamps reversed fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO '2018-01-01'
  WHERE id = '[3,4)';

-- Deleting with a subquery fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM (SELECT '2018-01-01') TO '2018-06-01'
  WHERE id = '[3,4)';

-- Deleting with a column fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM lower(valid_at) TO NULL
  WHERE id = '[3,4)';
-- and so does a whole-row reference, which takes a different path
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at ((ROW(for_portion_of_test.*)).f2)
  WHERE id = '[3,4)';

-- Deleting with timestamps equal does nothing
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-04-01'
  WHERE id = '[3,4)';

-- Deleting a finite/open portion with a finite/open target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-06-01' TO NULL
  WHERE id = '[3,4)';
SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

-- Deleting a finite/open portion with an open/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-03-01'
  WHERE id = '[6,7)';
SELECT * FROM for_portion_of_test WHERE id = '[6,7)' ORDER BY id, valid_at;

-- Deleting an open/finite portion with an open/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-02-01'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Deleting an open/finite portion with a finite/open target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2017-01-01' TO NULL
  WHERE id = '[7,8)';
SELECT * FROM for_portion_of_test WHERE id = '[7,8)' ORDER BY id, valid_at;

-- Deleting a finite/finite portion with an exact fit
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-04-01'
  WHERE id = '[4,5)';
SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

-- Deleting an enclosed span
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

-- Deleting an open/open portion with a finite/finite target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-01' TO '2019-01-01'
  WHERE id = '[5,6)';
SELECT * FROM for_portion_of_test WHERE id = '[5,6)' ORDER BY id, valid_at;

-- Deleting an enclosed span with separate protruding spans
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-03' TO '2018-03-03'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting multiple enclosed spans
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO NULL
  WHERE id = '[8,9)';
SELECT * FROM for_portion_of_test WHERE id = '[8,9)' ORDER BY id, valid_at;

-- Deleting with a direct target
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (daterange('2018-03-10', '2018-03-15'))
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting with a direct target, coerced from a string
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at ('[2018-03-15,2018-03-17)')
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- Deleting with a direct target of the wrong range subtype fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (int4range(1, 4))
  WHERE id = '[1,2)';

-- Deleting with a direct target of a non-rangetype fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (4)
  WHERE id = '[1,2)';

-- Deleting with a direct target of NULL fails
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at (NULL)
  WHERE id = '[1,2)';

-- Deleting with a direct target of empty does nothing
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at ('empty')
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

-- DELETE with no WHERE clause
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2030-01-01' TO NULL;

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;
\set QUIET true

-- UPDATE ... RETURNING returns only the updated values
-- (not the inserted side values, which are added by a separate "statement"):
CREATE FUNCTION fpo_returning_row(text)
RETURNS text LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE 'RETURNING %', $1;
  RETURN $1;
END;
$$;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-02-15'
  SET name = 'three^3'
  WHERE id = '[3,4)'
  RETURNING *, fpo_returning_row(for_portion_of_test::text);

-- UPDATE ... RETURNING supports NEW and OLD valid_at
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-10' TO '2018-02-20'
  SET name = 'three^4'
  WHERE id = '[3,4)'
  RETURNING OLD.name, NEW.name, OLD.valid_at, NEW.valid_at;

-- DELETE FOR PORTION OF with current_date
-- (We take care not to make the expectation depend on the timestamp.)
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[99,100)', '[2000-01-01,)', 'foo');
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM current_date TO null
  WHERE id = '[99,100)';
SELECT name, lower(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date - 1;
SELECT name, upper(valid_at) FROM for_portion_of_test
  WHERE id = '[99,100)' AND valid_at @> current_date + 1;

-- DELETE FOR PORTION OF with clock_timestamp()
-- fails because the function is volatile:
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM clock_timestamp()::date TO null
  WHERE id = '[99,100)';

-- clean up:
DELETE FROM for_portion_of_test WHERE id = '[99,100)';

-- DELETE ... RETURNING returns the deleted values, regardless of bounds
-- (not the inserted side values, which are added by a separate "statement"):
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-02-02' TO '2018-02-03'
  WHERE id = '[3,4)'
  RETURNING *, fpo_returning_row(for_portion_of_test::text);

-- DELETE FOR PORTION OF in a PL/pgSQL function
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[10,11)', '[2018-01-01,2020-01-01)', 'ten');
CREATE FUNCTION fpo_delete(_id int4range, _target_from date, _target_til date)
RETURNS void LANGUAGE plpgsql AS
$$
BEGIN
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at FROM $2 TO $3
    WHERE id = $1;
END;
$$;
SELECT fpo_delete('[10,11)', '2015-01-01', '2019-01-01');
SELECT * FROM for_portion_of_test WHERE id = '[10,11)';
DELETE FROM for_portion_of_test WHERE id IN ('[10,11)');

-- DELETE FOR PORTION OF in a compiled SQL function
CREATE FUNCTION fpo_delete()
RETURNS text
BEGIN ATOMIC
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
    RETURNING name;
END;
\sf+ fpo_delete()
CREATE OR REPLACE function fpo_delete()
RETURNS text
BEGIN ATOMIC
  DELETE FROM for_portion_of_test
    FOR PORTION OF valid_at (daterange('2018-01-15', '2020-01-01') * daterange('2019-01-01', '2022-01-01'))
    RETURNING name;
END;
\sf+ fpo_delete()
DROP FUNCTION fpo_delete();


-- test domains and CHECK constraints

-- With a domain on a rangetype
CREATE DOMAIN daterange_d AS daterange CHECK (upper(VALUE) <> '2005-05-05'::date);
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at daterange_d,
  name text
);
INSERT INTO for_portion_of_test2 VALUES
  (1, '[2000-01-01,2020-01-01)', 'one'),
  (2, '[2000-01-01,2020-01-01)', 'two');
INSERT INTO for_portion_of_test2 VALUES
  (1, '[2000-01-01,2005-05-05)', 'nope');
-- UPDATE works:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2010-01-01' TO '2010-01-05'
  SET name = 'one^1'
  WHERE id = 1;
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('[2010-01-07,2010-01-09)')
  SET name = 'one^2'
  WHERE id = 1;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- The target is allowed to violate the domain:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  SET name = 'miss'
  WHERE id = -1;
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('[1999-01-01,2005-05-05)')
  SET name = 'miss'
  WHERE id = -1;
-- test the updated row violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  SET name = 'one^3'
  WHERE id = 1;
-- test inserts violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2005-05-05' TO '2010-01-01'
  SET name = 'one^3'
  WHERE id = 1;
-- test updated row violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (upper(valid_at) <> '2001-01-11');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2001-01-11'
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (lower(valid_at) <> '2002-02-02');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-02-02'
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- DELETE works:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2010-01-01' TO '2010-01-05'
  WHERE id = 2;
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('[2010-01-07,2010-01-09)')
  WHERE id = 2;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
-- The target is allowed to violate the domain:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '1999-01-01' TO '2005-05-05'
  WHERE id = -1;
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('[1999-01-01,2005-05-05)')
  WHERE id = -1;
-- test inserts violating the domain
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2005-05-05' TO '2010-01-01'
  WHERE id = 2;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (lower(valid_at) <> '2002-02-02');
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-02-02'
  WHERE id = 2;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
DROP TABLE for_portion_of_test2;

-- The domain is checked for every leftover, not just the first one.  The
-- lookup is cached across rows, so make sure a violation is still caught on a
-- row after one that passed.
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at daterange_d,
  name text
);
INSERT INTO for_portion_of_test2 VALUES
  (1, '[2006-01-01,2020-01-01)', 'one'),
  (2, '[2000-01-01,2020-01-01)', 'two'),
  (3, '[2000-01-01,2020-01-01)', 'three');

-- Every row's leftovers are fine here: several rows, several checks.
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2010-01-01' TO '2011-01-01'
  SET name = name || '!';
SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;

-- Now the first row's leftovers pass but the second row's violate the domain.
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2005-05-05' TO '2007-01-01'
  SET name = 'nope';
SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;
DROP TABLE for_portion_of_test2;

-- With a domain on a multirangetype
CREATE FUNCTION multirange_lowers(mr anymultirange) RETURNS anyarray LANGUAGE sql AS $$
  SELECT array_agg(lower(r)) FROM UNNEST(mr) u(r);
$$;
CREATE FUNCTION multirange_uppers(mr anymultirange) RETURNS anyarray LANGUAGE sql AS $$
  SELECT array_agg(upper(r)) FROM UNNEST(mr) u(r);
$$;
CREATE DOMAIN datemultirange_d AS datemultirange CHECK (NOT '2005-05-05'::date = ANY (multirange_uppers(VALUE)));
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at datemultirange_d,
  name text
);
INSERT INTO for_portion_of_test2 VALUES
  (1, '{[2000-01-01,2020-01-01)}', 'one'),
  (2, '{[2000-01-01,2020-01-01)}', 'two');
INSERT INTO for_portion_of_test2 VALUES
  (1, '{[2000-01-01,2005-05-05)}', 'nope');
-- UPDATE works:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2010-01-07,2010-01-09)}')
  SET name = 'one^2'
  WHERE id = 1;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- The target is allowed to violate the domain:
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  SET name = 'miss'
  WHERE id = -1;
-- test the updated row violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  SET name = 'one^3'
  WHERE id = 1;
-- test inserts violating the domain
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2005-05-05,2010-01-01)}')
  SET name = 'one^3'
  WHERE id = 1;
-- test updated row violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (upper(valid_at) <> '2001-01-11');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2000-01-01,2001-01-11)}')
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (NOT '2002-02-02'::date = ANY (multirange_lowers(valid_at)));
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2001-01-01,2002-02-02)}')
  SET name = 'one^3'
  WHERE id = 1;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 1 ORDER BY valid_at;
-- DELETE works:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2010-01-07,2010-01-09)}')
  WHERE id = 2;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
-- The target is allowed to violate the domain:
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[1999-01-01,2005-05-05)}')
  WHERE id = -1;
-- test inserts violating the domain
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2005-05-05,2010-01-01)}')
  WHERE id = 2;
-- test inserts violating CHECK constraints
ALTER TABLE for_portion_of_test2
  ADD CONSTRAINT fpo2_check CHECK (NOT '2002-02-02'::date = ANY (multirange_lowers(valid_at)));
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2001-01-01,2002-02-02)}')
  WHERE id = 2;
ALTER TABLE for_portion_of_test2 DROP CONSTRAINT fpo2_check;
SELECT * FROM for_portion_of_test2 WHERE id = 2 ORDER BY valid_at;
DROP TABLE for_portion_of_test2;

-- test on non-range/multirange columns

-- With a direct target and a scalar column
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at date,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '2020-01-01', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('2010-01-01')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('2010-01-01');
DROP TABLE for_portion_of_test2;

-- With a direct target and a non-{,multi}range gistable column without overlaps
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at point,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '0,0', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('1,1')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('1,1');
DROP TABLE for_portion_of_test2;

-- With a direct target and a non-{,multi}range column with overlaps
CREATE TABLE for_portion_of_test2 (
  id integer,
  valid_at box,
  name text
);
INSERT INTO for_portion_of_test2 VALUES (1, '0,0,4,4', 'one');
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('1,1,2,2')
  SET name = 'one^1';
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('1,1,2,2');
DROP TABLE for_portion_of_test2;

-- test that we run triggers on the UPDATE/DELETEd row and the INSERTed rows

CREATE FUNCTION dump_trigger()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE '%: % % %:',
    TG_NAME, TG_WHEN, TG_OP, TG_LEVEL;

  IF TG_ARGV[0] THEN
    RAISE NOTICE '  old: %', (SELECT string_agg(old_table::text, '\n       ') FROM old_table);
  ELSE
    RAISE NOTICE '  old: %', OLD.valid_at;
  END IF;
  IF TG_ARGV[1] THEN
    RAISE NOTICE '  new: %', (SELECT string_agg(new_table::text, '\n       ') FROM new_table);
  ELSE
    RAISE NOTICE '  new: %', NEW.valid_at;
  END IF;

  IF TG_OP = 'INSERT' OR TG_OP = 'UPDATE' THEN
    RETURN NEW;
  ELSIF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
END;
$$;

-- statement triggers:

CREATE TRIGGER fpo_before_stmt
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_before_stmt1
  BEFORE UPDATE OF valid_at ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_stmt
  AFTER INSERT ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_update_stmt
  AFTER UPDATE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_delete_stmt
  AFTER DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

-- row triggers:

CREATE TRIGGER fpo_before_row
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_before_row1
  BEFORE UPDATE OF valid_at ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);


UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2021-01-01' TO '2022-01-01'
  SET name = 'five^3'
  WHERE id = '[5,6)';

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2023-01-01' TO '2024-01-01'
  WHERE id = '[5,6)';

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- Triggers with a custom transition table name:

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test VALUES ('[1,2)', '[2018-01-01,2020-01-01)', 'one');

-- statement triggers:

CREATE TRIGGER fpo_before_stmt
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_stmt
  AFTER INSERT ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, true);

CREATE TRIGGER fpo_after_update_stmt
  AFTER UPDATE ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(true, true);

CREATE TRIGGER fpo_after_delete_stmt
  AFTER DELETE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(true, false);

-- row triggers:

CREATE TRIGGER fpo_before_row
  BEFORE INSERT OR UPDATE OR DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  REFERENCING NEW TABLE AS new_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, true);

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table NEW TABLE AS new_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(true, true);

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  REFERENCING OLD TABLE AS old_table
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(true, false);

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = '2018-01-15_to_2019-01-01';
ROLLBACK;

BEGIN;
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-21';
ROLLBACK;

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-02'
  SET name = 'NULL_to_2018-01-01';
ROLLBACK;

-- Deferred triggers
-- (must be CONSTRAINT triggers thus AFTER ROW with no transition tables)

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-01,2020-01-01)', 'one');

CREATE CONSTRAINT TRIGGER fpo_after_insert_row
  AFTER INSERT ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE CONSTRAINT TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

CREATE CONSTRAINT TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2019-01-01'
  SET name = '2018-01-15_to_2019-01-01';
COMMIT;

BEGIN;
DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-21';
COMMIT;

BEGIN;
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM NULL TO '2018-01-02'
  SET name = 'NULL_to_2018-01-01';
COMMIT;

SELECT * FROM for_portion_of_test;

-- test FOR PORTION OF from triggers during FOR PORTION OF:

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO for_portion_of_test (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-01,2020-01-01)', 'one'),
  ('[2,3)', '[2018-01-01,2020-01-01)', 'two'),
  ('[3,4)', '[2018-01-01,2020-01-01)', 'three'),
  ('[4,5)', '[2018-01-01,2020-01-01)', 'four');

CREATE FUNCTION trg_fpo_update()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() = 1 THEN
    UPDATE for_portion_of_test
      FOR PORTION OF valid_at FROM '2018-02-01' TO '2018-03-01'
      SET name = CONCAT(name, '^')
      WHERE id = OLD.id;
  END IF;
  RETURN CASE WHEN 'TG_OP' = 'DELETE' THEN OLD ELSE NEW END;
END;
$$;

CREATE FUNCTION trg_fpo_delete()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() = 1 THEN
    DELETE FROM for_portion_of_test
      FOR PORTION OF valid_at FROM '2018-03-01' TO '2018-04-01'
      WHERE id = OLD.id;
  END IF;
  RETURN CASE WHEN 'TG_OP' = 'DELETE' THEN OLD ELSE NEW END;
END;
$$;

-- UPDATE FOR PORTION OF from a trigger fired by UPDATE FOR PORTION OF

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_update();

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '*')
  WHERE id = '[1,2)';

SELECT * FROM for_portion_of_test WHERE id = '[1,2)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_update_row ON for_portion_of_test;

-- UPDATE FOR PORTION OF from a trigger fired by DELETE FOR PORTION OF

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_update();

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test WHERE id = '[2,3)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_delete_row ON for_portion_of_test;

-- DELETE FOR PORTION OF from a trigger fired by UPDATE FOR PORTION OF

CREATE TRIGGER fpo_after_update_row
  AFTER UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_delete();

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '*')
  WHERE id = '[3,4)';

SELECT * FROM for_portion_of_test WHERE id = '[3,4)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_update_row ON for_portion_of_test;

-- DELETE FOR PORTION OF from a trigger fired by DELETE FOR PORTION OF

CREATE TRIGGER fpo_after_delete_row
  AFTER DELETE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_delete();

DELETE FROM for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  WHERE id = '[4,5)';

SELECT * FROM for_portion_of_test WHERE id = '[4,5)' ORDER BY id, valid_at;

DROP TRIGGER fpo_after_delete_row ON for_portion_of_test;

-- Test that a tuple-modifying BEFORE INSERT ROW trigger acts
-- consistently on both temporal leftovers.
-- When FOR PORTION OF splits a row into two leftovers, both triggers
-- should get the original row's values.

DROP TABLE for_portion_of_test;
CREATE TABLE for_portion_of_test (
  id int,
  valid_at daterange,
  name text
);

CREATE FUNCTION fpo_append_name_suffix()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  NEW.name := NEW.name || '+insert';
  RETURN NEW;
END;
$$;

CREATE TRIGGER fpo_before_insert_row
  BEFORE INSERT ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE fpo_append_name_suffix();

INSERT INTO for_portion_of_test VALUES (1, '[2020-01-01,2020-12-31)', 'foo');

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2020-04-01' TO '2020-08-01'
  SET name = 'bar'
  WHERE id = 1;

-- Both leftovers should have the same name: 'foo+insert+insert'.
SELECT * FROM for_portion_of_test ORDER BY valid_at;

DROP FUNCTION fpo_append_name_suffix CASCADE;
DROP TABLE for_portion_of_test;

-- A BEFORE UPDATE trigger that changes the application-time column is allowed,
-- even if the results are senseless.
-- Note this is likely to cause a primary key violation.

CREATE TABLE for_portion_of_test (
  id int4range,
  valid_at daterange,
  name text
);

CREATE FUNCTION trg_fpo_change_valid_at()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  NEW.valid_at = daterange('2018-01-01', '2019-01-01');
  RETURN NEW;
END;
$$;

CREATE TRIGGER fpo_before_update_row
  BEFORE UPDATE ON for_portion_of_test
  FOR EACH ROW EXECUTE PROCEDURE trg_fpo_change_valid_at();

INSERT INTO for_portion_of_test VALUES ('[1,2)', '[2010-01-01,2020-01-01)', 'foo');

UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '!')
  WHERE id = '[1,2)';

SELECT * FROM for_portion_of_test ORDER BY id, valid_at;

-- A primary key should reject anything invalid:
TRUNCATE for_portion_of_test;
ALTER TABLE for_portion_of_test
  ADD CONSTRAINT for_portion_of_test_key
  PRIMARY KEY (id, valid_at WITHOUT OVERLAPS);
INSERT INTO for_portion_of_test VALUES ('[1,2)', '[2010-01-01,2020-01-01)', 'foo');
UPDATE for_portion_of_test
  FOR PORTION OF valid_at FROM '2018-05-01' TO '2018-06-01'
  SET name = CONCAT(name, '!')
  WHERE id = '[1,2)';

DROP TRIGGER fpo_before_update_row ON for_portion_of_test;
DROP FUNCTION trg_fpo_change_valid_at();
DROP TABLE for_portion_of_test;

-- Test with multiranges

CREATE TABLE for_portion_of_test2 (
  id int4range NOT NULL,
  valid_at datemultirange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_test2_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', datemultirange(daterange('2018-01-02', '2018-02-03)'), daterange('2018-02-04', '2018-03-03')), 'one'),
  ('[1,2)', datemultirange(daterange('2018-03-03', '2018-04-04)')), 'one'),
  ('[2,3)', datemultirange(daterange('2018-01-01', '2018-05-01)')), 'two'),
  ('[3,4)', datemultirange(daterange('2018-01-01', null)), 'three');
  ;

-- Updating with FROM/TO
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2010-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';
-- Updating with multirange
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (datemultirange(daterange('2018-01-10', '2018-02-10'), daterange('2018-03-05', '2018-05-01')))
  SET name = 'one^1'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;
-- Updating with string coercion
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[2018-03-05,2018-03-10)}')
  SET name = 'one^2'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;
-- Updating with the wrong range subtype fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{[1,4)}'::int4multirange)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with a non-multirangetype fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (4)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with NULL fails
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at (NULL)
  SET name = 'one^3'
  WHERE id = '[1,2)';
-- Updating with empty does nothing
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at ('{}')
  SET name = 'one^3'
  WHERE id = '[1,2)';
SELECT * FROM for_portion_of_test2 WHERE id = '[1,2)' ORDER BY valid_at;

-- Deleting with FROM/TO
UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2000-01-01' TO '2010-01-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';
-- Deleting with multirange
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (datemultirange(daterange('2018-01-15', '2018-02-15'), daterange('2018-03-01', '2018-03-15')))
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test2 WHERE id = '[2,3)' ORDER BY valid_at;
-- Deleting with string coercion
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[2018-03-05,2018-03-20)}')
  WHERE id = '[2,3)';
SELECT * FROM for_portion_of_test2 WHERE id = '[2,3)' ORDER BY valid_at;
-- Deleting with the wrong range subtype fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{[1,4)}'::int4multirange)
  WHERE id = '[2,3)';
-- Deleting with a non-multirangetype fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (4)
  WHERE id = '[2,3)';
-- Deleting with NULL fails
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at (NULL)
  WHERE id = '[2,3)';
-- Deleting with empty does nothing
DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at ('{}')
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;

DROP TABLE for_portion_of_test2;

-- Test with a custom range type

CREATE TYPE mydaterange AS range(subtype=date);

CREATE TABLE for_portion_of_test2 (
  id int4range NOT NULL,
  valid_at mydaterange NOT NULL,
  name text NOT NULL,
  CONSTRAINT for_portion_of_test2_pk PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
INSERT INTO for_portion_of_test2 (id, valid_at, name) VALUES
  ('[1,2)', '[2018-01-02,2018-02-03)', 'one'),
  ('[1,2)', '[2018-02-03,2018-03-03)', 'one'),
  ('[1,2)', '[2018-03-03,2018-04-04)', 'one'),
  ('[2,3)', '[2018-01-01,2018-05-01)', 'two'),
  ('[3,4)', '[2018-01-01,)', 'three');
  ;

UPDATE for_portion_of_test2
  FOR PORTION OF valid_at FROM '2018-01-10' TO '2018-02-10'
  SET name = 'one^1'
  WHERE id = '[1,2)';

DELETE FROM for_portion_of_test2
  FOR PORTION OF valid_at FROM '2018-01-15' TO '2018-02-15'
  WHERE id = '[2,3)';

SELECT * FROM for_portion_of_test2 ORDER BY id, valid_at;

DROP TABLE for_portion_of_test2;
DROP TYPE mydaterange;

-- Test FOR PORTION OF against a partitioned table.
-- Include a stored generated column to test updatedCols column mapping.
-- temporal_partitioned_1 has the same attnums as the root
-- temporal_partitioned_3 has the different attnums from the root
-- temporal_partitioned_5 has the different attnums too, but reversed

CREATE TABLE temporal_partitioned (
  id int4range,
  valid_at daterange,
  name text,
  range_len int GENERATED ALWAYS AS (upper(valid_at) - lower(valid_at)) STORED,
  CONSTRAINT temporal_paritioned_uq UNIQUE (id, valid_at WITHOUT OVERLAPS)
) PARTITION BY LIST (id);
CREATE TABLE temporal_partitioned_1 PARTITION OF temporal_partitioned FOR VALUES IN ('[1,2)', '[2,3)');
CREATE TABLE temporal_partitioned_3 PARTITION OF temporal_partitioned FOR VALUES IN ('[3,4)', '[4,5)');
CREATE TABLE temporal_partitioned_5 PARTITION OF temporal_partitioned FOR VALUES IN ('[5,6)', '[6,7)');

ALTER TABLE temporal_partitioned DETACH PARTITION temporal_partitioned_3;
ALTER TABLE temporal_partitioned_3 DROP COLUMN id, DROP COLUMN valid_at CASCADE;
ALTER TABLE temporal_partitioned_3 ADD COLUMN id int4range NOT NULL, ADD COLUMN valid_at daterange NOT NULL;
ALTER TABLE temporal_partitioned_3 ADD COLUMN range_len int GENERATED ALWAYS AS (upper(valid_at) - lower(valid_at)) STORED;
ALTER TABLE temporal_partitioned ATTACH PARTITION temporal_partitioned_3 FOR VALUES IN ('[3,4)', '[4,5)');

ALTER TABLE temporal_partitioned DETACH PARTITION temporal_partitioned_5;
ALTER TABLE temporal_partitioned_5 DROP COLUMN id, DROP COLUMN valid_at CASCADE;
ALTER TABLE temporal_partitioned_5 ADD COLUMN valid_at daterange NOT NULL, ADD COLUMN id int4range NOT NULL;
ALTER TABLE temporal_partitioned_5 ADD COLUMN range_len int GENERATED ALWAYS AS (upper(valid_at) - lower(valid_at)) STORED;
ALTER TABLE temporal_partitioned ATTACH PARTITION temporal_partitioned_5 FOR VALUES IN ('[5,6)', '[6,7)');

INSERT INTO temporal_partitioned (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one'),
  ('[3,4)', daterange('2000-01-01', '2010-01-01'), 'three'),
  ('[5,6)', daterange('2000-01-01', '2010-01-01'), 'five');

SELECT * FROM temporal_partitioned;

-- Update without moving within partition 1
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'one^1'
  WHERE id = '[1,2)';

-- Update without moving within partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'three^1'
  WHERE id = '[3,4)';

-- Update without moving within partition 5
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-04-01'
  SET name = 'five^1'
  WHERE id = '[5,6)';

-- Move from partition 1 to partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'one^2',
      id = '[4,5)'
  WHERE id = '[1,2)'
  RETURNING id, valid_at, name, fpo_returning_row(temporal_partitioned::text);

-- Move from partition 3 to partition 1
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'three^2',
      id = '[2,3)'
  WHERE id = '[3,4)';

-- Move from partition 5 to partition 3
UPDATE temporal_partitioned FOR PORTION OF valid_at FROM '2000-06-01' TO '2000-07-01'
  SET name = 'five^2',
      id = '[3,4)'
  WHERE id = '[5,6)';

-- Update all partitions at once (each with leftovers)

SELECT *, upper(valid_at) - lower(valid_at) FROM temporal_partitioned ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_1 ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_3 ORDER BY id, valid_at;
SELECT * FROM temporal_partitioned_5 ORDER BY id, valid_at;

DROP FUNCTION fpo_returning_row;
DROP TABLE temporal_partitioned;

-- Test FOR PORTION OF when the partition key depends on the range column.
-- Then a leftover can belong to a partition that is not one of the plan's
-- result relations, and we must build a new ResultRelInfo for it.

CREATE TABLE temporal_partitioned_by_valid_at (
  id int4range,
  valid_at daterange,
  name text
) PARTITION BY RANGE (lower(valid_at));
CREATE TABLE temporal_partitioned_early
  PARTITION OF temporal_partitioned_by_valid_at
  FOR VALUES FROM (MINVALUE) TO ('2000-06-01');
CREATE TABLE temporal_partitioned_late
  PARTITION OF temporal_partitioned_by_valid_at
  FOR VALUES FROM ('2000-06-01') TO (MAXVALUE);

INSERT INTO temporal_partitioned_by_valid_at (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one');

-- The WHERE clause prunes away the late partition, but the second leftover
-- belongs there.  The leftovers must not appear in the RETURNING output.
DELETE FROM temporal_partitioned_by_valid_at
  FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-07-01'
  WHERE lower(valid_at) < '2000-06-01'
  RETURNING id, valid_at, name;
SELECT tableoid::regclass, * FROM temporal_partitioned_by_valid_at
  ORDER BY id, valid_at;

-- The same thing for UPDATE
DELETE FROM temporal_partitioned_by_valid_at;
INSERT INTO temporal_partitioned_by_valid_at (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one');
UPDATE temporal_partitioned_by_valid_at
  FOR PORTION OF valid_at FROM '2000-03-01' TO '2000-07-01'
  SET name = 'one^1'
  WHERE lower(valid_at) < '2000-06-01'
  RETURNING id, valid_at, name;
SELECT tableoid::regclass, * FROM temporal_partitioned_by_valid_at
  ORDER BY id, valid_at;

DROP TABLE temporal_partitioned_by_valid_at;

-- UPDATE/DELETE FOR PORTION OF with RULEs
CREATE TABLE fpo_rule (f1 bigint, f2 int4range);
INSERT INTO fpo_rule VALUES (1, '[1, 11)');

CREATE RULE fpo_rule1 AS ON INSERT TO fpo_rule
  DO INSTEAD UPDATE fpo_rule FOR PORTION OF f2 FROM 1 TO 4 SET f1 = 2;
INSERT INTO fpo_rule VALUES (1, '[1, 11)');
SELECT * FROM fpo_rule ORDER BY f1;

CREATE RULE fpo_rule2 AS ON INSERT TO fpo_rule
  DO INSTEAD DELETE FROM fpo_rule FOR PORTION OF f2 FROM 1 TO 4;
INSERT INTO fpo_rule VALUES (1, '[1, 11)');
SELECT * FROM fpo_rule ORDER BY f1;

CREATE RULE fpo_rule3 AS ON DELETE TO fpo_rule
  DO INSTEAD UPDATE fpo_rule FOR PORTION OF f2 FROM 1 TO 8 SET f1 = 2;
DELETE FROM fpo_rule FOR PORTION OF f2 FROM 1 TO 5;
SELECT * FROM fpo_rule ORDER BY f1;

DROP RULE fpo_rule3 ON fpo_rule;
CREATE RULE fpo_rule4 AS ON UPDATE TO fpo_rule
  DO INSTEAD DELETE FROM fpo_rule FOR PORTION OF f2 FROM 6 TO 9;
UPDATE fpo_rule FOR PORTION OF f2 FROM 4 TO 9 SET f1 = 12;
SELECT * FROM fpo_rule ORDER BY f1;

DROP RULE fpo_rule4 ON fpo_rule;
CREATE RULE fpo_rule5 AS ON UPDATE TO fpo_rule
  DO ALSO DELETE FROM fpo_rule FOR PORTION OF f2 FROM 4 TO 6;
UPDATE fpo_rule FOR PORTION OF f2 FROM 9 TO 10 SET f1 = 3;
SELECT * FROM fpo_rule ORDER BY f1;

DROP TABLE fpo_rule;

-- Deparsing FOR PORTION OF must use the range column's current name,
-- not the name it had when the rule was created.
CREATE TABLE fpo_rename (f1 bigint, f2 int4range);
CREATE TABLE fpo_rename_src (x int);
CREATE RULE fpo_rename_rule1 AS ON UPDATE TO fpo_rename_src
  DO INSTEAD UPDATE fpo_rename FOR PORTION OF f2 FROM 3 TO 6 SET f1 = 2;
CREATE RULE fpo_rename_rule2 AS ON DELETE TO fpo_rename_src
  DO INSTEAD DELETE FROM fpo_rename FOR PORTION OF f2 (int4range(3, 6));

\d+ fpo_rename_src
ALTER TABLE fpo_rename RENAME COLUMN f1 TO ff1;
ALTER TABLE fpo_rename RENAME COLUMN f2 TO ff2;
\d+ fpo_rename_src

DROP TABLE fpo_rename, fpo_rename_src;

-- Deparsed FROM/TO bounds must be re-loadable.  A bound ending in an interval
-- type name with a field qualifier would otherwise run into the following TO,
-- since "interval hour to minute" is a type name too.
CREATE TYPE fpo_intervalrange AS RANGE (subtype = interval);
CREATE TABLE fpo_interval (r fpo_intervalrange);
CREATE RULE fpo_interval_rule AS ON INSERT TO fpo_interval DO ALSO
  DELETE FROM fpo_interval
    FOR PORTION OF r FROM (INTERVAL '1' HOUR) TO INTERVAL '2' HOUR;
SELECT definition FROM pg_rules WHERE rulename = 'fpo_interval_rule';
SELECT 'DROP RULE fpo_interval_rule ON fpo_interval'
  UNION ALL
  SELECT pg_get_ruledef(oid) FROM pg_rewrite
    WHERE rulename = 'fpo_interval_rule' \gexec
SELECT definition FROM pg_rules WHERE rulename = 'fpo_interval_rule';

DROP TABLE fpo_interval;
DROP TYPE fpo_intervalrange;

-- UPDATE/DELETE FOR PORTION OF on a GENERATED VIRTUAL range column:
CREATE TABLE fpo_gen_virtual (
  a int,
  b int4range GENERATED ALWAYS AS (int4range(a, a + 1)) VIRTUAL
);
INSERT INTO fpo_gen_virtual VALUES (1);
DELETE FROM fpo_gen_virtual FOR PORTION OF b FROM 1 TO 2; -- fails
UPDATE fpo_gen_virtual FOR PORTION OF b FROM 1 TO 2 SET a = 5; -- fails
DROP TABLE fpo_gen_virtual;

-- UPDATE/DELETE FOR PORTION OF on a GENERATED STORED range column:
CREATE TABLE fpo_gen_stored (
  a int,
  b int4range GENERATED ALWAYS AS (int4range(a, a + 1)) STORED
);
INSERT INTO fpo_gen_stored VALUES (1);
DELETE FROM fpo_gen_stored FOR PORTION OF b FROM 1 TO 2; -- fails
UPDATE fpo_gen_stored FOR PORTION OF b FROM 1 TO 2 SET a = 5; -- fails
DROP TABLE fpo_gen_stored;

-- FOR PORTION OF a generated column reached through an updatable view.
-- The view hides that b is generated during parse analysis, so the check
-- must happen later (in the planner), after the view is rewritten to its
-- underlying table.
CREATE TABLE fpo_gen_view (
  a int,
  b int4range GENERATED ALWAYS AS (int4range(a, a + 1)) STORED
);
INSERT INTO fpo_gen_view VALUES (1);
CREATE VIEW fpo_gen_view_v AS SELECT * FROM fpo_gen_view;
DELETE FROM fpo_gen_view_v FOR PORTION OF b FROM 1 TO 2; -- fails
UPDATE fpo_gen_view_v FOR PORTION OF b FROM 1 TO 2 SET a = 5; -- fails
DROP VIEW fpo_gen_view_v;
DROP TABLE fpo_gen_view;

-- A new-style SQL function is parsed at CREATE FUNCTION time, but our
-- generated-column check is in the planner, so it sees the column's
-- current attgenerated when the function's plan is built at run time.
CREATE TABLE fpo_func_test (
  a int,
  b int4range GENERATED ALWAYS AS (int4range(a, a + 1)) STORED
);
INSERT INTO fpo_func_test VALUES (1);
-- Definition succeeds even though b is a generated column today.
CREATE FUNCTION fpo_delete() RETURNS void
  LANGUAGE SQL
  BEGIN ATOMIC
    DELETE FROM fpo_func_test FOR PORTION OF b FROM 1 TO 2;
  END;
SELECT fpo_delete(); -- fails: b is generated
-- Drop the generation expression and the same function now succeeds.
ALTER TABLE fpo_func_test ALTER COLUMN b DROP EXPRESSION;
SELECT fpo_delete();
TABLE fpo_func_test ORDER BY a, b;
DROP FUNCTION fpo_delete();
DROP TABLE fpo_func_test;

-- UPDATE/DELETE FOR PORTION OF with table inheritance
-- Leftover rows must stay in the child table, preserving child-specific columns.
CREATE TABLE fpo_inh_parent (
  id int4range,
  valid_at daterange,
  name text
);
CREATE TABLE fpo_inh_child (
  description text
) INHERITS (fpo_inh_parent);

-- Update targets the parent; the matching row lives in the child.
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');
UPDATE fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01'
  SET name = 'one^1';
-- All three rows should be in the child, with description preserved.
SELECT tableoid::regclass, * FROM fpo_inh_parent ORDER BY valid_at;
SELECT * FROM fpo_inh_child ORDER BY valid_at;
-- No rows should have leaked into the parent.
SELECT * FROM ONLY fpo_inh_parent ORDER BY valid_at;

-- Same test for DELETE instead of UPDATE:
TRUNCATE fpo_inh_child, fpo_inh_parent;
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');
DELETE FROM fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01';
-- Both rows should be in the child, with description preserved.
SELECT tableoid::regclass, * FROM fpo_inh_parent ORDER BY valid_at;
SELECT * FROM fpo_inh_child ORDER BY valid_at;
-- No rows should have leaked into the parent.
SELECT * FROM ONLY fpo_inh_parent ORDER BY valid_at;

-- Statement-level INSERT triggers for the leftovers must fire on the table
-- the leftovers are actually inserted into, i.e. the child, just as they
-- would for a plain INSERT into that child.
CREATE FUNCTION fpo_inh_stmt_trigger()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE '% % % on %', TG_NAME, TG_WHEN, TG_OP, TG_TABLE_NAME;
  RETURN NULL;
END;
$$;

CREATE TRIGGER fpo_inh_parent_bs BEFORE INSERT ON fpo_inh_parent
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_inh_parent_as AFTER INSERT ON fpo_inh_parent
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_inh_child_bs BEFORE INSERT ON fpo_inh_child
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_inh_child_as AFTER INSERT ON fpo_inh_child
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();

TRUNCATE fpo_inh_child, fpo_inh_parent;
-- control: a plain INSERT into the child fires only the child's triggers
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');

-- One firing per leftover, all of them on the child.
UPDATE fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01'
  SET name = 'one^1';
SELECT tableoid::regclass, * FROM fpo_inh_parent ORDER BY valid_at;

TRUNCATE fpo_inh_child, fpo_inh_parent;
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');
DELETE FROM fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01';
SELECT tableoid::regclass, * FROM fpo_inh_parent ORDER BY valid_at;

-- The transition table for those inserts belongs to the child too, and holds
-- the child's rows in the child's own format, including its extra columns.
CREATE FUNCTION fpo_inh_transition_trigger()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE '% on %: %', TG_NAME, TG_TABLE_NAME,
    (SELECT string_agg(new_table::text, ', ' ORDER BY new_table::text)
       FROM new_table);
  RETURN NULL;
END;
$$;

CREATE TRIGGER fpo_inh_parent_tt AFTER INSERT ON fpo_inh_parent
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_transition_trigger();
CREATE TRIGGER fpo_inh_child_tt AFTER INSERT ON fpo_inh_child
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_transition_trigger();

TRUNCATE fpo_inh_child, fpo_inh_parent;
-- control: a plain INSERT into the child reports description = 'initial'
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');
-- The leftovers must report it too: they are child rows and they keep it.
UPDATE fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01'
  SET name = 'one^1';
SELECT tableoid::regclass, * FROM fpo_inh_child ORDER BY valid_at;

-- DELETE inserts its leftovers the same way.
TRUNCATE fpo_inh_child, fpo_inh_parent;
INSERT INTO fpo_inh_child (id, valid_at, name, description) VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one', 'initial');
DELETE FROM fpo_inh_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01';
SELECT tableoid::regclass, * FROM fpo_inh_child ORDER BY valid_at;

DROP TABLE fpo_inh_parent CASCADE;

-- By contrast, leftovers in a partitioned table are inserted through the
-- root to get tuple routing, so the root is where their statement-level
-- triggers belong, exactly as for a plain INSERT into the root.
CREATE TABLE fpo_part_parent (
  id int4range,
  valid_at daterange,
  name text
) PARTITION BY RANGE (id);
CREATE TABLE fpo_part_child PARTITION OF fpo_part_parent
  FOR VALUES FROM ('[1,1]') TO ('[9,9]');

CREATE TRIGGER fpo_part_parent_bs BEFORE INSERT ON fpo_part_parent
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_part_parent_as AFTER INSERT ON fpo_part_parent
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_part_child_bs BEFORE INSERT ON fpo_part_child
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();
CREATE TRIGGER fpo_part_child_as AFTER INSERT ON fpo_part_child
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_stmt_trigger();

-- and so does their transition table, which still receives the rows converted
-- to the root's format.
CREATE FUNCTION fpo_part_transition_trigger()
RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  RAISE NOTICE '% on %: %', TG_NAME, TG_TABLE_NAME,
    (SELECT string_agg(new_table::text, ', ' ORDER BY new_table::text)
       FROM new_table);
  RETURN NULL;
END;
$$;
CREATE TRIGGER fpo_part_parent_tt AFTER INSERT ON fpo_part_parent
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_part_transition_trigger();

INSERT INTO fpo_part_parent VALUES
  ('[1,2)', '[2018-01-01,2019-01-01)', 'one');
UPDATE fpo_part_parent FOR PORTION OF valid_at FROM '2018-04-01' TO '2018-10-01'
  SET name = 'one^1';
SELECT tableoid::regclass, * FROM fpo_part_parent ORDER BY valid_at;
DROP FUNCTION fpo_part_transition_trigger();

DROP TABLE fpo_part_parent;
DROP FUNCTION fpo_inh_stmt_trigger();

-- UPDATE FOR PORTION OF with multiple inheritance
-- Leftover rows must stay in the child table, even if the range column's
-- attnum differs between the target parent and child.
CREATE TABLE temporal_parent (
  id int,
  valid_at daterange,
  name text
);
CREATE TABLE other_parent (
  prefix text,
  note text
);
CREATE TABLE mi_child () INHERITS (other_parent, temporal_parent);

-- The leftovers' transition table must use the child's format even when the
-- child's columns are in a different order from the parent's.
CREATE TRIGGER mi_child_tt AFTER INSERT ON mi_child
  REFERENCING NEW TABLE AS new_table
  FOR EACH STATEMENT EXECUTE PROCEDURE fpo_inh_transition_trigger();

-- attnum of the range column is different in temporal_parent and mi_child
SELECT attnum, attname
  FROM pg_attribute
  WHERE attrelid = 'temporal_parent'::regclass
    AND attnum > 0 AND NOT attisdropped
  ORDER BY attnum;
SELECT attnum, attname
  FROM pg_attribute
  WHERE attrelid = 'mi_child'::regclass
    AND attnum > 0 AND NOT attisdropped
  ORDER BY attnum;

INSERT INTO mi_child (prefix, note, id, valid_at, name) VALUES
  ('pfx', 'memo', 1, daterange('2000-01-01', '2010-01-01'), 'old');

UPDATE temporal_parent FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01'
  SET name = 'new'
  WHERE id = 1;

SELECT tableoid::regclass, * FROM temporal_parent ORDER BY valid_at;
SELECT * FROM mi_child ORDER BY valid_at;
SELECT * FROM ONLY temporal_parent ORDER BY valid_at;

TRUNCATE mi_child, other_parent, temporal_parent;
INSERT INTO mi_child (prefix, note, id, valid_at, name) VALUES
  ('pfx', 'memo', 1, daterange('2000-01-01', '2010-01-01'), 'old');

DELETE FROM temporal_parent FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01'
  WHERE id = 1;

SELECT tableoid::regclass, * FROM temporal_parent ORDER BY valid_at;
SELECT * FROM mi_child ORDER BY valid_at;
SELECT * FROM ONLY temporal_parent ORDER BY valid_at;

DROP TABLE temporal_parent CASCADE;
DROP FUNCTION fpo_inh_transition_trigger();

-- UPDATE FOR PORTION OF with generated columns
-- The generated column depends on the range column, so it must be
-- recomputed when FOR PORTION OF narrows the range.
CREATE TABLE fpo_generated (
  id int,
  valid_at int4range,
  range_len int GENERATED ALWAYS AS (upper(valid_at) - lower(valid_at)) STORED,
  range_lenv int GENERATED ALWAYS AS (upper(valid_at) - lower(valid_at))
);
INSERT INTO fpo_generated (id, valid_at) VALUES (1, '[10,100)');

SELECT * FROM fpo_generated ORDER BY valid_at;

-- After the FOR PORTION OF (FPO) update, all three resulting rows
-- (leftover-before, updated, and leftover-after) must contain the correct
-- values for range_len and range_lenv.
UPDATE fpo_generated
  FOR PORTION OF valid_at FROM 30 TO 70
  SET id = 2;

SELECT * FROM fpo_generated ORDER BY valid_at;

-- Also test with a generated column that references both a SET column
-- and the range column.
DROP TABLE fpo_generated;
CREATE TABLE fpo_generated (
  id int,
  valid_at int4range,
  id_plus_len int GENERATED ALWAYS AS (id + upper(valid_at) - lower(valid_at)) STORED,
  id_plus_lenv int GENERATED ALWAYS AS (id + upper(valid_at) - lower(valid_at))
);

INSERT INTO fpo_generated (id, valid_at) VALUES (1, '[10,100)');
SELECT * FROM fpo_generated ORDER BY valid_at;

UPDATE fpo_generated
  FOR PORTION OF valid_at FROM 30 TO 70
  SET id = 2;
SELECT * FROM fpo_generated ORDER BY valid_at;
DROP TABLE fpo_generated;

-- Test that UPDATE OF colname triggers fire if colname is valid_at:
CREATE TABLE fpo_update_of_trigger (
  id int,
  valid_at int4range
);
INSERT INTO fpo_update_of_trigger (id, valid_at) VALUES (1, '[10,100)');
CREATE TRIGGER fpo_before_row1
  BEFORE UPDATE OF valid_at ON fpo_update_of_trigger
  FOR EACH ROW EXECUTE PROCEDURE dump_trigger(false, false);
CREATE TRIGGER fpo_before_row2
  BEFORE UPDATE OF valid_at ON fpo_update_of_trigger
  FOR EACH STATEMENT EXECUTE PROCEDURE dump_trigger(false, false);
UPDATE fpo_update_of_trigger
  FOR PORTION OF valid_at FROM 30 TO 70
  SET id = 2;
DROP TABLE fpo_update_of_trigger;

-- CURSORs
CREATE TABLE fpo_cursed (
  id int,
  valid_at int4range
);
INSERT INTO fpo_cursed (id, valid_at) VALUES (1, '[10,100)');

-- UPDATE FOR PORTION OF is not permitted with a CURSOR:
BEGIN;
DECLARE fpo_cur CURSOR FOR SELECT * FROM fpo_cursed;
FETCH NEXT FROM fpo_cur;
UPDATE fpo_cursed
  FOR PORTION OF valid_at FROM 5 TO 6
  SET id = 2
  WHERE CURRENT OF fpo_cur;
ROLLBACK;

-- DELETE FOR PORTION OF is not permitted with a CURSOR:
BEGIN;
DECLARE fpo_cur CURSOR FOR SELECT * FROM fpo_cursed;
FETCH NEXT FROM fpo_cur;
DELETE FROM fpo_cursed
  FOR PORTION OF valid_at FROM 8 TO 9
  WHERE CURRENT OF fpo_cur;
ROLLBACK;
SELECT * FROM fpo_cursed;
DROP TABLE fpo_cursed;

-- UPDATE/DELETE FOR PORTION OF leftover rows must satisfy RLS INSERT checks.
CREATE ROLE regress_fpo_rls;
CREATE TABLE fpo_rls (
  id int,
  valid_at int4range
);
ALTER TABLE fpo_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY fpo_rls_select ON fpo_rls
  FOR SELECT TO regress_fpo_rls
  USING (true);
CREATE POLICY fpo_rls_update ON fpo_rls
  FOR UPDATE TO regress_fpo_rls
  USING (lower(valid_at) < 50)
  WITH CHECK (lower(valid_at) < 50);
CREATE POLICY fpo_rls_delete ON fpo_rls
  FOR DELETE TO regress_fpo_rls
  USING (lower(valid_at) < 50);
CREATE POLICY fpo_rls_insert ON fpo_rls
  FOR INSERT TO regress_fpo_rls
  WITH CHECK (lower(valid_at) < 50);
GRANT SELECT, UPDATE, DELETE ON fpo_rls TO regress_fpo_rls;

INSERT INTO fpo_rls VALUES (1, '[10,100)');
SET ROLE regress_fpo_rls;
UPDATE fpo_rls
  FOR PORTION OF valid_at FROM 30 TO 100
  SET id = 2;
RESET ROLE;
SELECT * FROM fpo_rls ORDER BY valid_at;

TRUNCATE fpo_rls;
INSERT INTO fpo_rls VALUES (1, '[10,100)');
SET ROLE regress_fpo_rls;
DELETE FROM fpo_rls
  FOR PORTION OF valid_at FROM 30 TO 100;
RESET ROLE;
SELECT * FROM fpo_rls ORDER BY valid_at;

TRUNCATE fpo_rls;
INSERT INTO fpo_rls VALUES (1, '[10,100)');
SET ROLE regress_fpo_rls;
UPDATE fpo_rls
  FOR PORTION OF valid_at FROM 30 TO 70
  SET id = 2;
RESET ROLE;
SELECT * FROM fpo_rls ORDER BY valid_at;

TRUNCATE fpo_rls;
INSERT INTO fpo_rls VALUES (1, '[10,100)');
SET ROLE regress_fpo_rls;
DELETE FROM fpo_rls
  FOR PORTION OF valid_at FROM 30 TO 70;
RESET ROLE;
SELECT * FROM fpo_rls ORDER BY valid_at;

DROP TABLE fpo_rls;
DROP ROLE regress_fpo_rls;

--
-- Parameters in the FOR PORTION OF bounds
--

CREATE TABLE fpo_param (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO fpo_param (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one');

-- A parameter of unspecified type in an ordinary expression gets its type
-- resolved from context.  This is the control case for the FROM/TO bounds
-- below: it builds exactly the same daterange the FROM/TO form does.
PREPARE fpo_param_control AS
  UPDATE fpo_param SET name = 'ctl' WHERE valid_at && daterange($1, $2);
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_control';

-- The (portion) form resolves the parameter type from the range column.
PREPARE fpo_param_portion AS
  UPDATE fpo_param FOR PORTION OF valid_at ($1) SET name = 'portion';
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_portion';

-- The FROM/TO form should likewise resolve its bounds to the range's
-- subtype, so that clients need not spell out the parameter types.
PREPARE fpo_param_update AS
  UPDATE fpo_param FOR PORTION OF valid_at FROM $1 TO $2 SET name = 'upd';
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_update';

PREPARE fpo_param_delete AS
  DELETE FROM fpo_param FOR PORTION OF valid_at FROM $1 TO $2;
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_delete';

-- Only one bound parameterized.
PREPARE fpo_param_one AS
  UPDATE fpo_param FOR PORTION OF valid_at FROM $1 TO '2003-01-01'
    SET name = 'one-bound';
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_one';

-- A parameter used as both bounds must still resolve to one type.
PREPARE fpo_param_same AS
  UPDATE fpo_param FOR PORTION OF valid_at FROM $1 TO $1 SET name = 'same';
SELECT parameter_types FROM pg_prepared_statements
  WHERE name = 'fpo_param_same';

EXECUTE fpo_param_update('2002-01-01', '2003-01-01');
SELECT * FROM fpo_param ORDER BY valid_at;

DEALLOCATE fpo_param_control;
DEALLOCATE fpo_param_portion;
DEALLOCATE fpo_param_update;
DEALLOCATE fpo_param_delete;
DEALLOCATE fpo_param_one;
DEALLOCATE fpo_param_same;

-- The bounds we keep for deparsing are the coerced ones, so an untyped NULL
-- bound renders with the range's subtype instead of "unknown".
CREATE TABLE fpo_param_src (id int);
CREATE RULE fpo_param_r AS ON DELETE TO fpo_param_src DO INSTEAD
  DELETE FROM fpo_param FOR PORTION OF valid_at FROM NULL TO '2001-01-01';
SELECT definition FROM pg_rules WHERE rulename = 'fpo_param_r';

DROP TABLE fpo_param_src;
DROP TABLE fpo_param;

--
-- Names in the FOR PORTION OF bounds
--
-- A bound may not read the row being changed, but a name in that position
-- doesn't necessarily mean a column: it may belong to a PL/pgSQL variable or
-- to a parameter of the SQL-bodied function we are part of.
--

CREATE TABLE fpo_name (valid_at int4range, name text);
INSERT INTO fpo_name VALUES ('[1,100)', 'one');

-- Columns of the target relation are still rejected, whether they are named
-- directly or reached through a whole-row reference.
UPDATE fpo_name FOR PORTION OF valid_at FROM lower(valid_at) TO 5
  SET name = 'nope';
UPDATE fpo_name FOR PORTION OF valid_at (valid_at) SET name = 'nope';
DELETE FROM fpo_name FOR PORTION OF valid_at ((ROW(fpo_name.*)).f1);
DELETE FROM fpo_name FOR PORTION OF valid_at FROM (ROW(fpo_name.*)).f2 TO 5;

-- PL/pgSQL variables may be bounds, under either #variable_conflict rule.
CREATE FUNCTION fpo_name_plpgsql(lo int, hi int) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  UPDATE fpo_name FOR PORTION OF valid_at FROM lo TO hi SET name = 'plpgsql';
END $$;
SELECT fpo_name_plpgsql(10, 20);

CREATE FUNCTION fpo_name_plpgsql_var(lo int, hi int) RETURNS void
LANGUAGE plpgsql AS $$
#variable_conflict use_variable
BEGIN
  UPDATE fpo_name FOR PORTION OF valid_at FROM lo TO hi SET name = 'variable';
END $$;
SELECT fpo_name_plpgsql_var(30, 40);
SELECT * FROM fpo_name ORDER BY valid_at;

-- A PL/pgSQL variable that shares its name with a column of the target is
-- still ambiguous, just as it would be anywhere else in the statement.
CREATE FUNCTION fpo_name_plpgsql_ambig(valid_at int) RETURNS void
LANGUAGE plpgsql AS $$
BEGIN
  UPDATE fpo_name FOR PORTION OF valid_at FROM valid_at TO 50 SET name = 'amb';
END $$;
SELECT fpo_name_plpgsql_ambig(45);

-- Parameters of a SQL-bodied function may be bounds too.  They deparse by
-- name, so pg_get_functiondef() output has to be re-loadable.
CREATE FUNCTION fpo_name_sql(lo int, hi int) RETURNS void
LANGUAGE SQL
BEGIN ATOMIC
  DELETE FROM fpo_name FOR PORTION OF valid_at FROM $1 TO $2;
END;
SELECT pg_get_functiondef('fpo_name_sql(int,int)'::regprocedure);
SELECT pg_get_functiondef('fpo_name_sql(int,int)'::regprocedure) \gexec
SELECT fpo_name_sql(60, 70);
SELECT * FROM fpo_name ORDER BY valid_at;

DROP FUNCTION fpo_name_plpgsql;
DROP FUNCTION fpo_name_plpgsql_var;
DROP FUNCTION fpo_name_plpgsql_ambig;
DROP FUNCTION fpo_name_sql;
DROP TABLE fpo_name;

-- The bounds see UPDATE's FROM list, as they see DELETE's USING list, so a
-- name resolves in them as it does in the rest of the statement, and a
-- column of another table is rejected like any other column.
CREATE TABLE fpo_from (id int, valid_at int4range, name text);
INSERT INTO fpo_from VALUES (1, '[1,100)', 'a');
CREATE TABLE fpo_from_src (lo int, hi int);
INSERT INTO fpo_from_src VALUES (5, 6);
UPDATE fpo_from FOR PORTION OF valid_at FROM fpo_from_src.lo TO fpo_from_src.hi
  SET name = 'x' FROM fpo_from_src; -- fails
UPDATE fpo_from FOR PORTION OF valid_at FROM lo TO hi
  SET name = 'x' FROM fpo_from_src; -- fails
DELETE FROM fpo_from FOR PORTION OF valid_at FROM lo TO hi
  USING fpo_from_src; -- fails
-- In PL/pgSQL, a variable named like a FROM/USING column is ambiguous in
-- the bounds of both.
CREATE FUNCTION fpo_from_upd(lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  UPDATE fpo_from FOR PORTION OF valid_at FROM lo TO hi SET name = 'u'
    FROM fpo_from_src WHERE fpo_from_src.lo = 5;
END $$;
CREATE FUNCTION fpo_from_del(lo int, hi int) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
  DELETE FROM fpo_from FOR PORTION OF valid_at FROM lo TO hi
    USING fpo_from_src WHERE fpo_from_src.lo = 5;
END $$;
SELECT fpo_from_upd(50, 60); -- fails
SELECT fpo_from_del(70, 80); -- fails
-- In a SQL function, the column hides the parameter, in the bounds as in SET.
CREATE FUNCTION fpo_from_sql(lo int) RETURNS void LANGUAGE sql AS $$
  UPDATE fpo_from FOR PORTION OF valid_at FROM lo TO lo + 1 SET name = 'u' || lo
    FROM fpo_from_src WHERE fpo_from.id = 1
$$; -- fails
SELECT * FROM fpo_from ORDER BY valid_at;
DROP FUNCTION fpo_from_upd, fpo_from_del;
DROP TABLE fpo_from, fpo_from_src;

--
-- EXPLAIN without ANALYZE must only plan the statement, so it must not
-- evaluate the FOR PORTION OF target.
--

CREATE TABLE fpo_explain (
  id int4range,
  valid_at daterange,
  name text
);
INSERT INTO fpo_explain (id, valid_at, name) VALUES
  ('[1,2)', daterange('2000-01-01', '2010-01-01'), 'one');

-- GENERIC_PLAN exists precisely so that a statement containing parameter
-- placeholders can be planned without supplying any parameter values.  The
-- hand-written equivalent qual is the control case.
EXPLAIN (COSTS OFF, GENERIC_PLAN)
  UPDATE fpo_explain SET name = 'q' WHERE valid_at && $1::daterange;
EXPLAIN (COSTS OFF, GENERIC_PLAN)
  UPDATE fpo_explain FOR PORTION OF valid_at ($1::daterange) SET name = 'q';
EXPLAIN (COSTS OFF, GENERIC_PLAN)
  DELETE FROM fpo_explain FOR PORTION OF valid_at ($1::daterange);

-- A null target is a run-time error, so plain EXPLAIN should still report
-- the plan.
EXPLAIN (COSTS OFF)
  UPDATE fpo_explain FOR PORTION OF valid_at (NULL::daterange) SET name = 'q';
EXPLAIN (COSTS OFF)
  DELETE FROM fpo_explain FOR PORTION OF valid_at (NULL::daterange);

-- Actually running it is still an error.
UPDATE fpo_explain FOR PORTION OF valid_at (NULL::daterange) SET name = 'q';
DELETE FROM fpo_explain FOR PORTION OF valid_at (NULL::daterange);

-- EXPLAIN ANALYZE does execute, so the null check must still fire there.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  UPDATE fpo_explain FOR PORTION OF valid_at (NULL::daterange) SET name = 'q';

UPDATE fpo_explain FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = 'q';
SELECT * FROM fpo_explain ORDER BY valid_at;

DROP TABLE fpo_explain;

-- Bounds may not contain aggregates, window functions, or set-returning functions:
CREATE TABLE fpo_bounds (id int, valid_at int4range);
INSERT INTO fpo_bounds VALUES (1, '[1,100)');
-- aggregate:
UPDATE fpo_bounds FOR PORTION OF valid_at FROM count(*) TO 10 SET id = 2;
-- window function:
UPDATE fpo_bounds FOR PORTION OF valid_at FROM row_number() OVER () TO 10 SET id = 2;
-- set-returning function:
UPDATE fpo_bounds FOR PORTION OF valid_at FROM generate_series(1, 2) TO 10 SET id = 2;
DROP TABLE fpo_bounds;

-- EXPLAIN of FOR PORTION OF (plan shape should be stable):
CREATE TABLE fpo_explain (id int, valid_at daterange, name text);
INSERT INTO fpo_explain VALUES (1, '[2000-01-01,2010-01-01)', 'a');
EXPLAIN (COSTS OFF) UPDATE fpo_explain
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01'
  SET name = 'b';
EXPLAIN (COSTS OFF) DELETE FROM fpo_explain
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01';
DROP TABLE fpo_explain;

-- Parameterized bounds via PREPARE/EXECUTE must keep working once a generic plan
-- is chosen (parameters are allowed in bounds even though volatile functions
-- are not):
CREATE TABLE fpo_prepared (id int, valid_at int4range, name text);
INSERT INTO fpo_prepared VALUES (1, '[1,100)', 'a');
PREPARE fpo_upd (int, int) AS
  UPDATE fpo_prepared FOR PORTION OF valid_at FROM $1 TO $2 SET name = 'b'
  WHERE id = 1;
EXECUTE fpo_upd(10, 20);
EXECUTE fpo_upd(30, 40);
EXECUTE fpo_upd(50, 60);
EXECUTE fpo_upd(70, 80);
EXECUTE fpo_upd(85, 90);
EXECUTE fpo_upd(92, 95);
SELECT valid_at, name FROM fpo_prepared ORDER BY valid_at;
DEALLOCATE fpo_upd;
DROP TABLE fpo_prepared;

-- Leftover rows must copy the original row's values for identity and
-- default-valued columns; those defaults must NOT re-fire for leftovers:
CREATE SEQUENCE fpo_seq;
CREATE TABLE fpo_defaults (
  id int GENERATED ALWAYS AS IDENTITY,
  seqval int DEFAULT nextval('fpo_seq'),
  valid_at int4range,
  name text
);
INSERT INTO fpo_defaults (valid_at, name) VALUES ('[1,100)', 'a');
UPDATE fpo_defaults FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
-- All three rows keep id=1 and seqval=1 (defaults did not re-fire):
SELECT id, seqval, valid_at, name FROM fpo_defaults ORDER BY valid_at;
-- The sequence did not advance past the single explicit insert:
SELECT last_value FROM fpo_seq;
DROP TABLE fpo_defaults;
DROP SEQUENCE fpo_seq;

-- A large (toasted) value must be preserved intact in leftovers:
CREATE TABLE fpo_toast (id int, valid_at int4range, big text);
INSERT INTO fpo_toast VALUES (1, '[1,100)', repeat('x', 100000));
UPDATE fpo_toast FOR PORTION OF valid_at FROM 40 TO 60 SET id = 2;
SELECT id, valid_at, length(big) FROM fpo_toast ORDER BY valid_at;
DROP TABLE fpo_toast;

-- FOR PORTION OF is not supported inside MERGE:
CREATE TABLE fpo_merge (id int, valid_at int4range, name text);
CREATE TABLE fpo_merge_src (id int);
MERGE INTO fpo_merge t
USING fpo_merge_src s ON t.id = s.id
WHEN MATCHED THEN
  UPDATE FOR PORTION OF valid_at FROM 1 TO 2 SET name = 'x';
DROP TABLE fpo_merge, fpo_merge_src;

-- Subtransaction rollback undoes a FOR PORTION OF, including its leftovers:
CREATE TABLE fpo_savepoint (id int, valid_at int4range, name text);
INSERT INTO fpo_savepoint VALUES (1, '[1,100)', 'a');
BEGIN;
SAVEPOINT sp;
UPDATE fpo_savepoint FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
SELECT valid_at, name FROM fpo_savepoint ORDER BY valid_at;
ROLLBACK TO SAVEPOINT sp;
SELECT valid_at, name FROM fpo_savepoint ORDER BY valid_at;
COMMIT;
DROP TABLE fpo_savepoint;

-- If a leftover insert fails, the whole statement rolls back and the target
-- row is not left truncated:
CREATE TABLE fpo_atomic (id int, valid_at int4range, name text);
INSERT INTO fpo_atomic VALUES (1, '[1,100)', 'a');
CREATE FUNCTION fpo_reject_insert() RETURNS trigger LANGUAGE plpgsql AS
  $$ BEGIN RAISE EXCEPTION 'no leftovers allowed'; END $$;
CREATE TRIGGER fpo_atomic_trg BEFORE INSERT ON fpo_atomic
  FOR EACH ROW EXECUTE PROCEDURE fpo_reject_insert();
UPDATE fpo_atomic FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
-- The original row is intact (not truncated to [1,40)):
SELECT * FROM fpo_atomic ORDER BY valid_at;
DROP TABLE fpo_atomic;
DROP FUNCTION fpo_reject_insert();

-- Degenerate targets: no matching row, target entirely outside, target covering:
CREATE TABLE fpo_edge (id int, valid_at int4range, name text);
INSERT INTO fpo_edge VALUES (1, '[10,20)', 'a');
-- WHERE matches nothing: no change, no leftovers:
UPDATE fpo_edge FOR PORTION OF valid_at FROM 12 TO 15 SET name = 'b' WHERE id = 999;
SELECT * FROM fpo_edge ORDER BY valid_at;
-- Target range entirely outside the row's range: no change, no leftovers:
UPDATE fpo_edge FOR PORTION OF valid_at FROM 100 TO 200 SET name = 'b';
SELECT * FROM fpo_edge ORDER BY valid_at;
-- Target range fully covers the row: whole-row update, no leftovers:
UPDATE fpo_edge FOR PORTION OF valid_at FROM 0 TO 50 SET name = 'c';
SELECT * FROM fpo_edge ORDER BY valid_at;
DROP TABLE fpo_edge;

-- FOR PORTION OF after DROP COLUMN and after a dropped-then-readded column
-- (leftover tuple construction must handle attnum gaps):
CREATE TABLE fpo_dropcol (id int, junk int, valid_at int4range, name text);
INSERT INTO fpo_dropcol VALUES (1, 42, '[1,100)', 'a');
ALTER TABLE fpo_dropcol DROP COLUMN junk;
UPDATE fpo_dropcol FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
SELECT id, valid_at, name FROM fpo_dropcol ORDER BY valid_at;
ALTER TABLE fpo_dropcol ADD COLUMN extra text DEFAULT 'z';
UPDATE fpo_dropcol FOR PORTION OF valid_at FROM 10 TO 20 SET name = 'c';
SELECT id, valid_at, name, extra FROM fpo_dropcol ORDER BY valid_at;
DROP TABLE fpo_dropcol;

-- FOR PORTION OF applied directly to a partition (not the root):
CREATE TABLE fpo_proot (id int, valid_at int4range, name text) PARTITION BY LIST (id);
CREATE TABLE fpo_pchild PARTITION OF fpo_proot FOR VALUES IN (1);
INSERT INTO fpo_proot VALUES (1, '[1,100)', 'a');
UPDATE fpo_pchild FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
SELECT * FROM fpo_proot ORDER BY valid_at;
DROP TABLE fpo_proot;

-- A leftover that routes to no partition raises an error (and the whole
-- statement rolls back).  Partition by the range's lower bound so that narrowing
-- valid_at can push a leftover out of every existing partition.
CREATE TABLE fpo_part_err (id int, valid_at int4range, name text)
  PARTITION BY RANGE (lower(valid_at));
CREATE TABLE fpo_part_err_low PARTITION OF fpo_part_err
  FOR VALUES FROM (MINVALUE) TO (50);
INSERT INTO fpo_part_err VALUES (1, '[10,100)', 'a');
-- The leftover [60,100) has lower bound 60, which no partition covers:
UPDATE fpo_part_err FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'b';
-- The original row is intact (statement rolled back atomically):
SELECT * FROM fpo_part_err ORDER BY valid_at;
DROP TABLE fpo_part_err;

-- FOR PORTION OF against a table referenced by a temporal foreign key (NO ACTION).
-- UPDATE preserves total coverage via leftovers, so the FK stays satisfied even
-- when the target overlaps a range the child needs; DELETE removes coverage and
-- so is rejected when the child depends on it.
CREATE TABLE fpo_fk_parent (
  id int4range,
  valid_at daterange,
  note text,
  PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
CREATE TABLE fpo_fk_child (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES fpo_fk_parent (id, PERIOD valid_at)
);
INSERT INTO fpo_fk_parent VALUES ('[1,2)', '[2000-01-01,2010-01-01)', 'a');
INSERT INTO fpo_fk_child VALUES ('[1,2)', '[2000-01-01,2005-01-01)', '[1,2)');
-- ok: leftovers restore the coverage the child needs:
UPDATE fpo_fk_parent
  FOR PORTION OF valid_at FROM '2001-01-01' TO '2002-01-01'
  SET note = 'b';
SELECT * FROM fpo_fk_parent ORDER BY valid_at;
-- error: DELETE removes coverage the child depends on:
DELETE FROM fpo_fk_parent
  FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01';
-- ok: DELETE outside the child's needed range:
DELETE FROM fpo_fk_parent
  FOR PORTION OF valid_at FROM '2008-01-01' TO '2009-01-01';
SELECT * FROM fpo_fk_parent ORDER BY valid_at;
DROP TABLE fpo_fk_child, fpo_fk_parent;

-- FOR PORTION OF on the *referencing* side of a temporal foreign key: leftovers
-- only narrow the child's own validity, so they always still satisfy the FK.
CREATE TABLE fpo_ref_parent (
  id int4range,
  valid_at daterange,
  PRIMARY KEY (id, valid_at WITHOUT OVERLAPS)
);
CREATE TABLE fpo_ref_child (
  id int4range,
  valid_at daterange,
  parent_id int4range,
  note text,
  FOREIGN KEY (parent_id, PERIOD valid_at) REFERENCES fpo_ref_parent (id, PERIOD valid_at)
);
INSERT INTO fpo_ref_parent VALUES ('[1,2)', '[2000-01-01,2010-01-01)');
INSERT INTO fpo_ref_child VALUES ('[1,2)', '[2000-01-01,2008-01-01)', '[1,2)', 'a');
-- UPDATE FOR PORTION OF the child: leftovers still reference covered parent ranges:
UPDATE fpo_ref_child
  FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET note = 'b';
SELECT id, valid_at, parent_id, note FROM fpo_ref_child ORDER BY valid_at;
-- DELETE FOR PORTION OF the child: removes part of the child's validity (FK-safe):
DELETE FROM fpo_ref_child
  FOR PORTION OF valid_at FROM '2004-01-01' TO '2005-01-01';
SELECT id, valid_at, parent_id, note FROM fpo_ref_child ORDER BY valid_at;
DROP TABLE fpo_ref_child, fpo_ref_parent;

-- A NOT NULL non-range column is copied into leftovers (not nulled out):
CREATE TABLE fpo_notnull (id int, valid_at int4range, name text NOT NULL);
INSERT INTO fpo_notnull VALUES (1, '[1,100)', 'keep');
UPDATE fpo_notnull FOR PORTION OF valid_at FROM 40 TO 60 SET id = 2;
SELECT * FROM fpo_notnull ORDER BY valid_at;
DROP TABLE fpo_notnull;

-- A non-default collation on a non-range column is preserved in leftovers:
CREATE TABLE fpo_collate (id int, valid_at int4range, name text COLLATE "C");
INSERT INTO fpo_collate VALUES (1, '[1,100)', 'Xyz');
UPDATE fpo_collate FOR PORTION OF valid_at FROM 40 TO 60 SET id = 2;
SELECT id, valid_at, name FROM fpo_collate ORDER BY valid_at;
DROP TABLE fpo_collate;

-- EXPLAIN ANALYZE executes the FOR PORTION OF (including its leftovers):
CREATE TABLE fpo_ea (id int, valid_at daterange, name text);
INSERT INTO fpo_ea VALUES (1, '[2000-01-01,2010-01-01)', 'a');
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
  UPDATE fpo_ea FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01' SET name = 'b';
SELECT valid_at, name FROM fpo_ea ORDER BY valid_at;
DROP TABLE fpo_ea;

-- RLS USING visibility: FOR PORTION OF only sees and splits rows the USING
-- policy makes visible; rows hidden by USING are left untouched.  Runs as a
-- non-superuser role, since superusers bypass RLS.  The leftovers keep the
-- tenant column, so they satisfy the policy's implied WITH CHECK.
CREATE ROLE regress_fpo_rls_using;
CREATE TABLE fpo_rls_using (id int, valid_at int4range, tenant name, name text);
INSERT INTO fpo_rls_using VALUES
  (1, '[1,100)', 'regress_fpo_rls_using', 'a'),
  (2, '[1,100)', 'other', 'b');
ALTER TABLE fpo_rls_using ENABLE ROW LEVEL SECURITY;
CREATE POLICY fpo_rls_using_pol ON fpo_rls_using USING (tenant = current_user);
GRANT SELECT, INSERT, UPDATE ON fpo_rls_using TO regress_fpo_rls_using;
SET ROLE regress_fpo_rls_using;
-- Only the row whose tenant = current_user is visible, so only it is split:
UPDATE fpo_rls_using FOR PORTION OF valid_at FROM 40 TO 60 SET name = 'a2';
SELECT id, valid_at, tenant, name FROM fpo_rls_using ORDER BY id, valid_at;
RESET ROLE;
-- As the RLS-bypassing owner: the 'other' row was never seen or touched:
SELECT id, valid_at, tenant, name FROM fpo_rls_using ORDER BY id, valid_at;
DROP TABLE fpo_rls_using;
DROP ROLE regress_fpo_rls_using;

--
-- A user-defined default GiST operator class whose opcintype is a concrete
-- range (or multirange) type, rather than the polymorphic anyrange the
-- built-in range_ops uses.
--

CREATE TYPE fpo_range AS RANGE (
  subtype = date,
  multirange_type_name = fpo_multirange
);

CREATE FUNCTION fpo_range_overlaps(fpo_range, fpo_range) RETURNS boolean
  LANGUAGE sql IMMUTABLE STRICT
  AS $$ SELECT $1 OPERATOR(pg_catalog.&&) $2 $$;
CREATE OPERATOR && (
  leftarg = fpo_range, rightarg = fpo_range,
  procedure = fpo_range_overlaps, commutator = &&,
  restrict = rangesel, join = areajoinsel
);
CREATE OPERATOR CLASS fpo_range_ops
  DEFAULT FOR TYPE fpo_range USING gist AS
  OPERATOR 3 &&(fpo_range, fpo_range),
  FUNCTION 1 range_gist_consistent(internal, anyrange, smallint, oid, internal),
  FUNCTION 2 range_gist_union(internal, internal),
  FUNCTION 5 range_gist_penalty(internal, internal, internal),
  FUNCTION 6 range_gist_picksplit(internal, internal),
  FUNCTION 7 range_gist_same(anyrange, anyrange, internal);
-- gistutil.c looks this one up with get_opfamily_proc(opfamily, ANYOID,
-- ANYOID, GIST_TRANSLATE_CMPTYPE_PROC), so it must be registered that way.
ALTER OPERATOR FAMILY fpo_range_ops USING gist
  ADD FUNCTION 12 ("any", "any") gist_translate_cmptype_common(integer);

CREATE FUNCTION fpo_multirange_overlaps(fpo_multirange, fpo_multirange)
  RETURNS boolean LANGUAGE sql IMMUTABLE STRICT
  AS $$ SELECT $1 OPERATOR(pg_catalog.&&) $2 $$;
CREATE OPERATOR && (
  leftarg = fpo_multirange, rightarg = fpo_multirange,
  procedure = fpo_multirange_overlaps, commutator = &&,
  restrict = multirangesel, join = areajoinsel
);
CREATE OPERATOR CLASS fpo_multirange_ops
  DEFAULT FOR TYPE fpo_multirange USING gist AS
  OPERATOR 3 &&(fpo_multirange, fpo_multirange),
  FUNCTION 1 multirange_gist_consistent(internal, anymultirange, smallint, oid, internal),
  FUNCTION 2 range_gist_union(internal, internal),
  FUNCTION 3 multirange_gist_compress(internal),
  FUNCTION 5 range_gist_penalty(internal, internal, internal),
  FUNCTION 6 range_gist_picksplit(internal, internal),
  FUNCTION 7 range_gist_same(anyrange, anyrange, internal);
ALTER OPERATOR FAMILY fpo_multirange_ops USING gist
  ADD FUNCTION 12 ("any", "any") gist_translate_cmptype_common(integer);

CREATE TABLE fpo_opc (
  id int,
  valid_at fpo_range,
  valid_ats fpo_multirange,
  name text
);
-- The opclasses really are usable for indexing.
CREATE INDEX fpo_opc_r_idx ON fpo_opc USING gist (valid_at);
CREATE INDEX fpo_opc_mr_idx ON fpo_opc USING gist (valid_ats);

INSERT INTO fpo_opc VALUES
  (1, fpo_range('2000-01-01', '2010-01-01'),
      fpo_multirange(fpo_range('2000-01-01', '2010-01-01')), 'one');

-- control: an ordinary overlaps qual against the same opclass
UPDATE fpo_opc SET name = 'ctl'
  WHERE valid_at && fpo_range('2002-01-01', '2003-01-01');
SELECT * FROM fpo_opc ORDER BY valid_at;

UPDATE fpo_opc FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = 'upd';
SELECT id, valid_at, name FROM fpo_opc ORDER BY valid_at;
DELETE FROM fpo_opc FOR PORTION OF valid_at FROM '2005-01-01' TO '2006-01-01';
SELECT id, valid_at, name FROM fpo_opc ORDER BY valid_at;

DELETE FROM fpo_opc;
INSERT INTO fpo_opc VALUES
  (1, fpo_range('2000-01-01', '2010-01-01'),
      fpo_multirange(fpo_range('2000-01-01', '2010-01-01')), 'one');

UPDATE fpo_opc
  FOR PORTION OF valid_ats
    (fpo_multirange(fpo_range('2002-01-01', '2003-01-01')))
  SET name = 'upd';
SELECT id, valid_ats, name FROM fpo_opc ORDER BY valid_ats;
DELETE FROM fpo_opc
  FOR PORTION OF valid_ats
    (fpo_multirange(fpo_range('2005-01-01', '2006-01-01')));
SELECT id, valid_ats, name FROM fpo_opc ORDER BY valid_ats;

DROP TABLE fpo_opc;
DROP OPERATOR CLASS fpo_multirange_ops USING gist;
DROP OPERATOR && (fpo_multirange, fpo_multirange);
DROP FUNCTION fpo_multirange_overlaps(fpo_multirange, fpo_multirange);
DROP OPERATOR CLASS fpo_range_ops USING gist;
DROP OPERATOR && (fpo_range, fpo_range);
DROP FUNCTION fpo_range_overlaps(fpo_range, fpo_range);
DROP TYPE fpo_range;

--
-- Diagnostics when a trigger on the leftover INSERTs changes rows the
-- statement has not reached yet.
--
-- Leftovers are inserted as the scan proceeds and fire their own triggers, so
-- an AFTER row trigger can trip the "already modified by an operation
-- triggered by the current command" check.  The stock hint only mentions
-- BEFORE triggers, so we add a detail naming the other possibility.
--

CREATE TABLE fpo_selfmod (
  id int,
  valid_at daterange,
  name text,
  touched int NOT NULL DEFAULT 0
);
INSERT INTO fpo_selfmod (id, valid_at, name) VALUES
  (1, daterange('2000-01-01', '2010-01-01'), 'one'),
  (2, daterange('2000-01-01', '2010-01-01'), 'two');

CREATE FUNCTION fpo_touch_siblings() RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() > 1 THEN
    RETURN NULL;
  END IF;
  UPDATE fpo_selfmod SET touched = touched + 1 WHERE id <> NEW.id;
  RETURN NULL;
END;
$$;

CREATE TRIGGER fpo_selfmod_ai AFTER INSERT ON fpo_selfmod
  FOR EACH ROW EXECUTE PROCEDURE fpo_touch_siblings();

UPDATE fpo_selfmod FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = name || '!';
DELETE FROM fpo_selfmod FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01';

-- The same detail appears for the check in GetTupleForTrigger(), which a
-- BEFORE trigger reaches.  There the stock hint is the right advice, so it is
-- still what we suggest.
CREATE FUNCTION fpo_touch_siblings_bu() RETURNS TRIGGER LANGUAGE plpgsql AS
$$
BEGIN
  IF pg_trigger_depth() = 1 THEN
    UPDATE fpo_selfmod SET touched = touched + 1 WHERE id <> NEW.id;
  END IF;
  RETURN NEW;
END;
$$;
CREATE TRIGGER fpo_selfmod_bu BEFORE UPDATE ON fpo_selfmod
  FOR EACH ROW EXECUTE PROCEDURE fpo_touch_siblings_bu();
UPDATE fpo_selfmod FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = name || '!';
DROP TRIGGER fpo_selfmod_bu ON fpo_selfmod;
DROP FUNCTION fpo_touch_siblings_bu();

-- Deferring the trigger works.
DROP TRIGGER fpo_selfmod_ai ON fpo_selfmod;
CREATE CONSTRAINT TRIGGER fpo_selfmod_ai AFTER INSERT ON fpo_selfmod
  DEFERRABLE INITIALLY DEFERRED
  FOR EACH ROW EXECUTE PROCEDURE fpo_touch_siblings();

BEGIN;
UPDATE fpo_selfmod FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = name || '!';
COMMIT;
SELECT * FROM fpo_selfmod ORDER BY id, valid_at;

DROP TABLE fpo_selfmod;
DROP FUNCTION fpo_touch_siblings();

--
-- WITH CHECK OPTION on a view
--
-- The row the statement produced is checked first, so a violation names the
-- row the user asked for and not an untouched leftover.
--

CREATE TABLE fpo_wco (
  id int,
  valid_at daterange,
  name text
);
INSERT INTO fpo_wco VALUES
  (1, daterange('2000-01-01', '2010-01-01'), 'keepme');

CREATE VIEW fpo_wco_v AS
  SELECT * FROM fpo_wco WHERE valid_at @> '2005-01-01'::date
  WITH CHECK OPTION;

-- Both the updated row and the leftovers fall outside the view.  The updated
-- row is the one the user wrote, so that is the one to report.
UPDATE fpo_wco_v FOR PORTION OF valid_at FROM '2002-01-01' TO '2003-01-01'
  SET name = 'changed';

-- Here the updated row still satisfies the view and only a leftover does not,
-- so the leftover is correctly the one reported.
UPDATE fpo_wco_v FOR PORTION OF valid_at FROM '2004-01-01' TO '2006-01-01'
  SET name = 'changed';

-- Nothing was written by either statement.
SELECT * FROM fpo_wco ORDER BY valid_at;

-- A portion covering the whole row leaves no leftovers and is accepted.
UPDATE fpo_wco_v FOR PORTION OF valid_at FROM '2000-01-01' TO '2010-01-01'
  SET name = 'changed';
SELECT * FROM fpo_wco ORDER BY valid_at;

DROP VIEW fpo_wco_v;
DROP TABLE fpo_wco;

--
-- Assigning the range column through a view that exposes it twice
--
-- transformUpdateTargetList() compares the names the user wrote, so it does
-- not notice that two view columns are the same base column.  The rewriter
-- must still say why the assignment is rejected.
--

CREATE TABLE fpo_dup (
  id int,
  valid_at daterange,
  name text
);
INSERT INTO fpo_dup VALUES
  (1, daterange('2000-01-01', '2010-01-01'), 'one');

CREATE VIEW fpo_dup_v AS
  SELECT id, valid_at AS a, valid_at AS b, name, name AS c, name AS d
  FROM fpo_dup;

UPDATE fpo_dup_v FOR PORTION OF a FROM '2002-01-01' TO '2003-01-01'
  SET b = daterange('1990-01-01', '1991-01-01');

-- Naming the same view column in both places is caught in parse analysis, as
-- it is for a table.
UPDATE fpo_dup_v FOR PORTION OF a FROM '2002-01-01' TO '2003-01-01'
  SET a = daterange('1990-01-01', '1991-01-01');

-- Two view columns aliasing a column that is not the FOR PORTION OF column
-- collide with each other, not with the clause, so they still get the generic
-- error: the new check must not fire too eagerly.
UPDATE fpo_dup_v FOR PORTION OF a FROM '2002-01-01' TO '2003-01-01'
  SET c = 'x', d = 'y';

-- Not assigning the range column through the view still works.
UPDATE fpo_dup_v FOR PORTION OF a FROM '2002-01-01' TO '2003-01-01'
  SET name = 'one^1';
SELECT * FROM fpo_dup ORDER BY valid_at;

DROP VIEW fpo_dup_v;
DROP TABLE fpo_dup;

RESET datestyle;
