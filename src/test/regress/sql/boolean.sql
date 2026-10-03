--
-- BOOLEAN
--

--
-- sanity check - if this fails go insane!
--
SELECT 1 AS one;


-- ******************testing built-in type bool********************

-- check bool input syntax

SELECT true AS true;

SELECT false AS false;

SELECT bool 't' AS true;

SELECT bool '   f           ' AS false;

SELECT bool 'true' AS true;

SELECT bool 'test' AS error;

SELECT bool 'false' AS false;

SELECT bool 'foo' AS error;

SELECT bool 'y' AS true;

SELECT bool 'yes' AS true;

SELECT bool 'yeah' AS error;

SELECT bool 'n' AS false;

SELECT bool 'no' AS false;

SELECT bool 'nay' AS error;

SELECT bool 'on' AS true;

SELECT bool 'off' AS false;

SELECT bool 'of' AS false;

SELECT bool 'o' AS error;

SELECT bool 'on_' AS error;

SELECT bool 'off_' AS error;

SELECT bool '1' AS true;

SELECT bool '11' AS error;

SELECT bool '0' AS false;

SELECT bool '000' AS error;

SELECT bool '' AS error;

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('true', 'bool');
SELECT pg_input_is_valid('asdf', 'bool');
SELECT * FROM pg_input_error_info('junk', 'bool');

-- and, or, not in qualifications

SELECT bool 't' or bool 'f' AS true;

SELECT bool 't' and bool 'f' AS false;

SELECT not bool 'f' AS true;

SELECT bool 't' = bool 'f' AS false;

SELECT bool 't' <> bool 'f' AS true;

SELECT bool 't' > bool 'f' AS true;

SELECT bool 't' >= bool 'f' AS true;

SELECT bool 'f' < bool 't' AS true;

SELECT bool 'f' <= bool 't' AS true;

-- explicit casts to/from text
SELECT 'TrUe'::text::boolean AS true, 'fAlse'::text::boolean AS false;
SELECT '    true   '::text::boolean AS true,
       '     FALSE'::text::boolean AS false;
SELECT true::boolean::text AS true, false::boolean::text AS false;

SELECT '  tru e '::text::boolean AS invalid;    -- error
SELECT ''::text::boolean AS invalid;            -- error

CREATE TABLE BOOLTBL1 (f1 bool);

INSERT INTO BOOLTBL1 (f1) VALUES (bool 't');

INSERT INTO BOOLTBL1 (f1) VALUES (bool 'True');

INSERT INTO BOOLTBL1 (f1) VALUES (bool 'true');


-- BOOLTBL1 should be full of true's at this point
SELECT BOOLTBL1.* FROM BOOLTBL1;


SELECT BOOLTBL1.*
   FROM BOOLTBL1
   WHERE f1 = bool 'true';


SELECT BOOLTBL1.*
   FROM BOOLTBL1
   WHERE f1 <> bool 'false';

SELECT BOOLTBL1.*
   FROM BOOLTBL1
   WHERE booleq(bool 'false', f1);

INSERT INTO BOOLTBL1 (f1) VALUES (bool 'f');

SELECT BOOLTBL1.*
   FROM BOOLTBL1
   WHERE f1 = bool 'false';


CREATE TABLE BOOLTBL2 (f1 bool);

INSERT INTO BOOLTBL2 (f1) VALUES (bool 'f');

INSERT INTO BOOLTBL2 (f1) VALUES (bool 'false');

INSERT INTO BOOLTBL2 (f1) VALUES (bool 'False');

INSERT INTO BOOLTBL2 (f1) VALUES (bool 'FALSE');

-- This is now an invalid expression
-- For pre-v6.3 this evaluated to false - thomas 1997-10-23
INSERT INTO BOOLTBL2 (f1)
   VALUES (bool 'XXX');

-- BOOLTBL2 should be full of false's at this point
SELECT BOOLTBL2.* FROM BOOLTBL2;


SELECT BOOLTBL1.*, BOOLTBL2.*
   FROM BOOLTBL1, BOOLTBL2
   WHERE BOOLTBL2.f1 <> BOOLTBL1.f1;


SELECT BOOLTBL1.*, BOOLTBL2.*
   FROM BOOLTBL1, BOOLTBL2
   WHERE boolne(BOOLTBL2.f1,BOOLTBL1.f1);


SELECT BOOLTBL1.*, BOOLTBL2.*
   FROM BOOLTBL1, BOOLTBL2
   WHERE BOOLTBL2.f1 = BOOLTBL1.f1 and BOOLTBL1.f1 = bool 'false';


SELECT BOOLTBL1.*, BOOLTBL2.*
   FROM BOOLTBL1, BOOLTBL2
   WHERE BOOLTBL2.f1 = BOOLTBL1.f1 or BOOLTBL1.f1 = bool 'true'
   ORDER BY BOOLTBL1.f1, BOOLTBL2.f1;

--
-- SQL syntax
-- Try all combinations to ensure that we get nothing when we expect nothing
-- - thomas 2000-01-04
--

SELECT f1
   FROM BOOLTBL1
   WHERE f1 IS TRUE;

SELECT f1
   FROM BOOLTBL1
   WHERE f1 IS NOT FALSE;

SELECT f1
   FROM BOOLTBL1
   WHERE f1 IS FALSE;

SELECT f1
   FROM BOOLTBL1
   WHERE f1 IS NOT TRUE;

SELECT f1
   FROM BOOLTBL2
   WHERE f1 IS TRUE;

SELECT f1
   FROM BOOLTBL2
   WHERE f1 IS NOT FALSE;

SELECT f1
   FROM BOOLTBL2
   WHERE f1 IS FALSE;

SELECT f1
   FROM BOOLTBL2
   WHERE f1 IS NOT TRUE;

--
-- Tests for BooleanTest
--
CREATE TABLE BOOLTBL3 (d text, b bool, o int);
INSERT INTO BOOLTBL3 (d, b, o) VALUES ('true', true, 1);
INSERT INTO BOOLTBL3 (d, b, o) VALUES ('false', false, 2);
INSERT INTO BOOLTBL3 (d, b, o) VALUES ('null', null, 3);

SELECT
    d,
    b IS TRUE AS istrue,
    b IS NOT TRUE AS isnottrue,
    b IS FALSE AS isfalse,
    b IS NOT FALSE AS isnotfalse,
    b IS UNKNOWN AS isunknown,
    b IS NOT UNKNOWN AS isnotunknown
FROM booltbl3 ORDER BY o;


-- Test to make sure short-circuiting and NULL handling is
-- correct. Use a table as source to prevent constant simplification
-- from interfering.
CREATE TABLE booltbl4(isfalse bool, istrue bool, isnul bool);
INSERT INTO booltbl4 VALUES (false, true, null);
\pset null '(null)'

-- AND expression need to return null if there's any nulls and not all
-- of the value are true
SELECT istrue AND isnul AND istrue FROM booltbl4;
SELECT istrue AND istrue AND isnul FROM booltbl4;
SELECT isnul AND istrue AND istrue FROM booltbl4;
SELECT isfalse AND isnul AND istrue FROM booltbl4;
SELECT istrue AND isfalse AND isnul FROM booltbl4;
SELECT isnul AND istrue AND isfalse FROM booltbl4;

-- OR expression need to return null if there's any nulls and none
-- of the value is true
SELECT isfalse OR isnul OR isfalse FROM booltbl4;
SELECT isfalse OR isfalse OR isnul FROM booltbl4;
SELECT isnul OR isfalse OR isfalse FROM booltbl4;
SELECT isfalse OR isnul OR istrue FROM booltbl4;
SELECT istrue OR isfalse OR isnul FROM booltbl4;
SELECT isnul OR istrue OR isfalse FROM booltbl4;

-- Implication: "a IMPLIES b" is "NOT a OR b".  It is not commutative, so all
-- nine combinations of three-valued logic have to be checked.
SELECT istrue IMPLIES istrue, istrue IMPLIES isfalse, istrue IMPLIES isnul
  FROM booltbl4;
SELECT isfalse IMPLIES istrue, isfalse IMPLIES isfalse, isfalse IMPLIES isnul
  FROM booltbl4;
SELECT isnul IMPLIES istrue, isnul IMPLIES isfalse, isnul IMPLIES isnul
  FROM booltbl4;

-- the same, as constants, so that constant folding is exercised too
SELECT true IMPLIES true, true IMPLIES false, true IMPLIES null;
SELECT false IMPLIES true, false IMPLIES false, false IMPLIES null;
SELECT null IMPLIES true, null IMPLIES false, null::bool IMPLIES null;

-- IMPLIES is non-associative, so a chain is refused rather than grouped
SELECT isfalse IMPLIES istrue IMPLIES isfalse FROM booltbl4;    -- error

-- and it has to be, because the two groupings are different formulas.  The
-- first row below is the interesting one: it tells them apart.
SELECT a, b, c,
       (a IMPLIES b) IMPLIES c AS grouped_left,
       a IMPLIES (b IMPLIES c) AS grouped_right,
       (a AND b) IMPLIES c AS conj_antecedent,
       a IMPLIES (b AND c) AS conj_consequent
  FROM (VALUES (false, false, false),
               (true, false, false),
               (true, true, true)) AS t(a, b, c);

-- grouping to the right is implication from the conjunction of the operands
-- (exportation), which holds for all three truth values, so no rows here
SELECT a, b, c
  FROM (VALUES (true), (false), (null)) AS x(a),
       (VALUES (true), (false), (null)) AS y(b),
       (VALUES (true), (false), (null)) AS z(c)
 WHERE (a IMPLIES (b IMPLIES c)) IS DISTINCT FROM ((a AND b) IMPLIES c);

-- and binds looser than OR, AND, NOT, the comparison operators and IS
SELECT istrue OR isfalse IMPLIES isfalse FROM booltbl4;
SELECT isfalse IMPLIES isfalse AND isfalse FROM booltbl4;
SELECT NOT istrue IMPLIES istrue FROM booltbl4;
SELECT 1 = 1 IMPLIES 2 = 3;
SELECT isfalse IMPLIES isnul IS NULL FROM booltbl4;

-- non-boolean operands are reported in terms of IMPLIES, not of its expansion
SELECT 1 IMPLIES true;                  -- error
SELECT true IMPLIES 1;                  -- error

-- stored expressions keep IMPLIES, and deparse with only the parentheses
-- that are needed
CREATE VIEW boolview AS
  SELECT istrue IMPLIES isnul AS i,
         NOT istrue IMPLIES isnul AS not_antecedent,
         istrue OR isfalse IMPLIES isnul AND istrue AS or_and,
         (istrue IMPLIES isfalse) IMPLIES isnul AS grouped_left,
         istrue IMPLIES (isfalse IMPLIES isnul) AS grouped_right,
         (istrue IMPLIES isfalse) OR isnul AS under_or,
         NOT (istrue IMPLIES isfalse) AS under_not,
         (istrue IMPLIES isfalse) IS TRUE AS under_is
    FROM booltbl4;
SELECT pg_get_viewdef('boolview', true);
SELECT pg_get_viewdef('boolview', false);
SELECT * FROM boolview;
DROP VIEW boolview;

CREATE TABLE implies_check (a int, b int,
  CHECK (a > 0 IMPLIES b > 0));
SELECT pg_get_constraintdef(oid) FROM pg_constraint
 WHERE conrelid = 'implies_check'::regclass;
INSERT INTO implies_check VALUES (1, 1), (0, 0), (NULL, 0), (1, NULL);
INSERT INTO implies_check VALUES (1, 0);    -- error

-- the planner expands it, so EXPLAIN shows NOT a OR b, simplified
EXPLAIN (COSTS OFF, VERBOSE)
SELECT * FROM implies_check WHERE a > 0 IMPLIES b > 0;
EXPLAIN (COSTS OFF)
SELECT * FROM implies_check WHERE a IS NULL IMPLIES false;

-- and a partial index on IMPLIES is usable for the expanded form
CREATE INDEX implies_check_idx ON implies_check (b)
  WHERE a IS NULL IMPLIES b > 0;
SELECT pg_get_indexdef('implies_check_idx'::regclass);
SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT b FROM implies_check WHERE a IS NOT NULL OR b > 0;
RESET enable_seqscan;
DROP TABLE implies_check;

-- IMPLIES is unreserved, so it remains usable as an identifier
CREATE TABLE implies (implies bool);
INSERT INTO implies VALUES (false);
SELECT implies IMPLIES implies FROM implies;
DROP TABLE implies;
SELECT 1 AS implies;
SELECT 1 implies;

-- Casts
SELECT 0::boolean;
SELECT 1::boolean;
SELECT 2::boolean;


--
-- Clean up
-- Many tables are retained by the regression test, but these do not seem
--  particularly useful so just get rid of them for now.
--  - thomas 1997-11-30
--

DROP TABLE  BOOLTBL1;

DROP TABLE  BOOLTBL2;

DROP TABLE  BOOLTBL3;

DROP TABLE  BOOLTBL4;
