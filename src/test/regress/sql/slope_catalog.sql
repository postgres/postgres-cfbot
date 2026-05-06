--
-- SLOPE catalog: verify prosupport assignments for monotonic functions
--

-- Operators with slope prosupport
SELECT
    o.oid AS "oid",
    o.oprname AS operator,
    format_type(o.oprleft, NULL) COLLATE "C" AS left_type,
    format_type(o.oprright, NULL) COLLATE "C" AS right_type,
    sp.proname AS prosupport
FROM pg_operator o
JOIN pg_proc p ON p.oid = o.oprcode
JOIN pg_proc sp ON sp.oid = p.prosupport
WHERE sp.proname LIKE '%slope%'
ORDER BY sp.proname, o.oprname, left_type, right_type;

-- Functions (non-operator) with slope prosupport
SELECT
    p.oid AS "oid",
    p.proname AS "function",
    pg_get_function_arguments(p.oid) COLLATE "C" AS arguments,
    format_type(p.prorettype, NULL) AS returns,
    sp.proname AS prosupport
FROM pg_proc p
JOIN pg_proc sp ON sp.oid = p.prosupport
WHERE sp.proname LIKE '%slope%'
  AND NOT EXISTS (SELECT 1 FROM pg_operator o WHERE o.oprcode = p.oid)
ORDER BY sp.proname, p.proname, arguments;

-- Operators whose name has slope support for some types but not others
SELECT
    u.oid AS "oid",
    u.oprname AS operator,
    format_type(u.oprleft, NULL) COLLATE "C" AS left_type,
    format_type(u.oprright, NULL) COLLATE "C" AS right_type
FROM pg_operator u
JOIN pg_proc up ON up.oid = u.oprcode
WHERE (up.prosupport = 0 OR NOT EXISTS (
        SELECT 1 FROM pg_proc sp
        WHERE sp.oid = up.prosupport AND sp.proname LIKE '%slope%'))
  AND EXISTS (
      SELECT 1
      FROM pg_operator s
      JOIN pg_proc sp_impl ON sp_impl.oid = s.oprcode
      JOIN pg_proc sp_sup ON sp_sup.oid = sp_impl.prosupport
      WHERE s.oprname = u.oprname
        AND sp_sup.proname LIKE '%slope%')
ORDER BY u.oprname, left_type, right_type;

-- Functions whose name has slope support for some signatures but not others
SELECT
    u.oid AS "oid",
    u.proname AS "function",
    pg_get_function_arguments(u.oid) COLLATE "C" AS arguments,
    format_type(u.prorettype, NULL) AS returns
FROM pg_proc u
WHERE (u.prosupport = 0 OR NOT EXISTS (
        SELECT 1 FROM pg_proc sp
        WHERE sp.oid = u.prosupport AND sp.proname LIKE '%slope%'))
  AND NOT EXISTS (SELECT 1 FROM pg_operator o WHERE o.oprcode = u.oid)
  AND EXISTS (
      SELECT 1
      FROM pg_proc s
      JOIN pg_proc sp ON sp.oid = s.prosupport
      WHERE s.proname = u.proname
        AND sp.proname LIKE '%slope%'
        AND NOT EXISTS (SELECT 1 FROM pg_operator o WHERE o.oprcode = s.oid))
ORDER BY u.proname, arguments;
