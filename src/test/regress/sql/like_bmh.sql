--
-- LIKE BMH optimization
--
-- Exercise the fast path for simple contains patterns ('%literal%') and the
-- fallback cases that must preserve existing LIKE semantics.
--

-- Use table values so the planner cannot fold the LIKE expressions to Consts.
CREATE TEMP TABLE like_bmh_text_data(id int, val text);
INSERT INTO like_bmh_text_data VALUES
	(1, ''),
	(2, 'abc'),
	(3, 'abcd'),
	(4, 'xabcd'),
	(5, 'abcdx'),
	(6, 'xabcdx'),
	(7, 'abxd'),
	(8, NULL);

SELECT id, val LIKE '%abcd%' AS matched
FROM like_bmh_text_data
ORDER BY id;

-- Exercise escape removal in both eligible and short fallback literals.
CREATE TEMP TABLE like_bmh_escape_data(id int, val text);
INSERT INTO like_bmh_escape_data VALUES
	(1, 'xxa%cdexx'),
	(2, 'xxa%zzzxx'),
	(3, 'xxa_cdexx'),
	(4, 'xxa\cdexx'),
	(5, 'xxa%cxx');

SELECT id, val LIKE '%a\%cde%' AS matched
FROM like_bmh_escape_data WHERE id IN (1, 2) ORDER BY id;
SELECT id, val LIKE '%a\_cde%' AS matched
FROM like_bmh_escape_data WHERE id = 3;
SELECT id, val LIKE '%a\\cde%' AS matched
FROM like_bmh_escape_data WHERE id = 4;
SELECT id, val LIKE '%a#%cde%' ESCAPE '#' AS matched
FROM like_bmh_escape_data WHERE id = 1;
SELECT id, val LIKE '%a\%c%' AS matched
FROM like_bmh_escape_data WHERE id = 5;

-- Check the threshold and the main pattern-shape fallback cases.
SELECT val LIKE '%abc%' AS len3_fallback,
       val LIKE '%abcd%' AS len4_bmh
FROM like_bmh_text_data WHERE id = 6;
SELECT val LIKE 'abcd' AS exact_fallback
FROM like_bmh_text_data WHERE id = 3;
SELECT val LIKE 'abcd%' AS prefix_fallback
FROM like_bmh_text_data WHERE id = 5;
SELECT val LIKE '%abcd' AS suffix_fallback
FROM like_bmh_text_data WHERE id = 4;
SELECT val LIKE '%%' AS empty_literal_fallback
FROM like_bmh_text_data WHERE id = 6;
SELECT val LIKE '%%abcd%%' AS extra_percent_fallback
FROM like_bmh_text_data WHERE id = 6;
SELECT val LIKE '%abc\%' AS escaped_trailing_percent_fallback
FROM (VALUES ('abc%')) AS v(val);

-- name input goes through the namelike entry point.
CREATE TEMP TABLE like_bmh_name_data(id int, val name);
INSERT INTO like_bmh_name_data VALUES
	(1, 'xxclassxx'),
	(2, 'xxotherxx'),
	(3, NULL);
SELECT id, val LIKE '%class%' AS matched
FROM like_bmh_name_data
ORDER BY id;

-- Row-varying patterns must use the generic matcher.
CREATE TEMP TABLE like_bmh_patterns(p text);
INSERT INTO like_bmh_patterns VALUES
	('%abcd%'), ('%wxyz%'), ('%b_d%'), ('%b%e%');
SELECT p, 'xxabcdxx' LIKE p AS matched
FROM like_bmh_patterns
ORDER BY p COLLATE "C";

-- A ScalarArrayOpExpr must not reuse state across different array elements.
SELECT val LIKE ANY (ARRAY['%abcd%', '%wxyz%']) AS text_any
FROM (VALUES ('xxwxyzxx')) AS v(val);
SELECT val LIKE ALL (ARRAY['%abcd%', '%wxyz%']) AS text_all
FROM (VALUES ('xxabcdxx')) AS v(val);
SELECT val LIKE ANY (ARRAY['%abcd%', NULL, '%wxyz%']) AS text_any_null
FROM (VALUES ('xxwxyzxx')) AS v(val);
SELECT val::name LIKE ANY (ARRAY['%other%', '%class%']) AS name_any
FROM (VALUES ('xxclassxx')) AS v(val);

-- A custom plan sees a Const pattern; a generic plan sees a stable Param.
SET plan_cache_mode = force_custom_plan;
PREPARE like_bmh_text(text) AS
	SELECT id, val LIKE $1 AS matched
	FROM like_bmh_text_data WHERE id IN (6, 7) ORDER BY id;
EXECUTE like_bmh_text('%abcd%');
EXECUTE like_bmh_text('%wxyz%');
DEALLOCATE like_bmh_text;
RESET plan_cache_mode;

SET plan_cache_mode = force_generic_plan;
PREPARE like_bmh_text(text) AS
	SELECT id, val LIKE $1 AS matched
	FROM like_bmh_text_data WHERE id IN (6, 7) ORDER BY id;
EXECUTE like_bmh_text('%abcd%');
EXECUTE like_bmh_text('%wxyz%');
DEALLOCATE like_bmh_text;

PREPARE like_bmh_name(text) AS
	SELECT id, val LIKE $1 AS matched
	FROM like_bmh_name_data WHERE id IN (1, 2) ORDER BY id;
EXECUTE like_bmh_name('%class%');
EXECUTE like_bmh_name('%other%');
DEALLOCATE like_bmh_name;
RESET plan_cache_mode;
