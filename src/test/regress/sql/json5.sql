-- JSON5 type input validation
--
-- The json5 input function uses the non-incremental (recursive descent)
-- parser, so this file is also what exercises that parser's JSON5
-- handling; the incremental parser is covered by the test_json_parser
-- module.

-- valid JSON is valid JSON5
SELECT '{"key": "value"}'::json5;
SELECT '[1, 2, 3]'::json5;
SELECT '"hello"'::json5;
SELECT '42'::json5;
SELECT 'true'::json5;
SELECT 'null'::json5;

-- unquoted keys
SELECT '{key: "value"}'::json5;
SELECT '{_key: "value"}'::json5;
SELECT '{$key: "value"}'::json5;

-- reserved words as unquoted keys
SELECT '{true: 1, false: 2, null: 3}'::json5;

-- trailing commas
SELECT '{"key": "value",}'::json5;
SELECT '[1, 2, 3,]'::json5;

-- comments
SELECT E'// line comment\n{"key": "value"}'::json5;
SELECT '/* block comment */ {"key": "value"}'::json5;
SELECT E'{"key": /* inline */ "value"}'::json5;

-- single-quoted strings
SELECT $json5${'key': 'value'}$json5$::json5;
SELECT $json5$["single's quote"]$json5$::json5;

-- number extensions
SELECT '{"a": .5}'::json5;
SELECT '{"a": 5.}'::json5;
SELECT '{"a": 0xFF}'::json5;
SELECT '{"a": +1}'::json5;
SELECT '{"a": Infinity}'::json5;
SELECT '{"a": -Infinity}'::json5;
SELECT '{"a": NaN}'::json5;

-- multi-line strings
SELECT E'{"key": "line1\\\nline2"}'::json5;

-- unquoted identifiers are only valid as keys, not values (all error)
SELECT '{key: value}'::json5;
SELECT '{"key": undefined}'::json5;
SELECT '[a]'::json5;
SELECT 'undefined'::json5;

-- invalid JSON5 (should error)
SELECT '{"key": }'::json5;
SELECT ''::json5;
SELECT '[1,,]'::json5;
SELECT '{1a: 1}'::json5;
SELECT '0x'::json5;
SELECT '.'::json5;
SELECT '010'::json5;
SELECT '080'::json5;
SELECT '-00'::json5;
SELECT '1e2.3'::json5;
SELECT '/* only a comment */'::json5;
SELECT '/* unterminated'::json5;
SELECT $json5$'unterminated$json5$::json5;

-- unterminated block comments error even after a complete value
SELECT '1 /*'::json5;
SELECT '1 /**'::json5;
SELECT '1 /*/'::json5;
SELECT '[1] /* {"garbage" [[['::json5;
SELECT '1 /**/ /*'::json5;
SELECT '1 /**/'::json5;

-- line comments end at CR as well as LF
SELECT E'[1, // c\r2\n]'::json5::jsonb;
SELECT E'// c\r1'::json5::json;
SELECT E'1 // c\rgarbage'::json5;

-- signed NaN
SELECT '-NaN'::json5;
SELECT '+NaN'::json5;
SELECT '{"a": -NaN}'::json5::json;   -- should ERROR
SELECT '{"a": +NaN}'::json5::jsonb;  -- should ERROR

-- Infinity and NaN are identifiers, so also valid as unquoted keys
SELECT '{Infinity: 1, NaN: 2}'::json5;
SELECT '{Infinity: 1, NaN: 2}'::json5::json;
SELECT '{Infinity: 1, NaN: 2}'::json5::jsonb;
SELECT '{-Infinity: 1}'::json5;      -- should ERROR (not an identifier)

-- \u0000 is valid json, so the json5 -> json cast must preserve it
SELECT '"\u0000"'::json5::json;
SELECT '{"a": "\u0000"}'::json5::json;
-- but combined with json5-only syntax the rebuild path still rejects it
SELECT '{a: "\u0000"}'::json5::json; -- should ERROR
-- jsonb rejects \u0000 itself, the cast stays consistent with that
SELECT '"\u0000"'::json5::jsonb;     -- should ERROR

-- verbatim storage: output preserves original formatting
SELECT $json5${key: 'value', /* comment */}$json5$::json5;
SELECT E'// comment\n[1, 2,]'::json5;

-- cast: json5 -> json (normalizes)
SELECT '{"key": "value"}'::json5::json;
SELECT $json5${key: 'value'}$json5$::json5::json;
SELECT '{true: 1, null: 2}'::json5::json;
SELECT '{"a": .5}'::json5::json;
SELECT '{"a": 5.}'::json5::json;
SELECT '{"a": 0xFF}'::json5::json;
SELECT '{"a": +1}'::json5::json;
SELECT '{"a": 6.022e23}'::json5::json;
-- decimal forms are rewritten textually, not limited by numeric's range
SELECT '[-.5, +.5e3, 5.e-3, -5., -0x10, +0XfF]'::json5::json;
-- e inside a hex literal is a digit, not an exponent marker
SELECT '[0xc8e4]'::json5::json;
SELECT '[.5e-1000000, 1.e400000]'::json5::json;
-- escapes in unquoted keys and json5-only string escapes
SELECT $json5${\u0061b: 1, s: '\x41\v\q\''}$json5$::json5::json;
SELECT $json5${\u0061b: 1, s: '\x41\v\q\''}$json5$::json5::jsonb;
SELECT '[1, 2, 3,]'::json5::json;
SELECT E'// c\n{"a": [1, /* c */ 2]}'::json5::json;
SELECT E'{"key": "line1\\\nline2"}'::json5::json;
-- containers as array elements need separating commas
SELECT '[[1], [2], {"a": [3, {"b": 4}]}]'::json5::json;
-- the rebuild keeps duplicate keys, like json does
SELECT '{a: 1, a: 2}'::json5::json;
SELECT '[{"a": 1}, {"b": 2}, 3, [4]]'::json5::json;
SELECT '{"key": Infinity}'::json5::json;   -- should ERROR
SELECT '{"key": -Infinity}'::json5::json;  -- should ERROR
SELECT '{"key": +Infinity}'::json5::json;  -- should ERROR
SELECT '{"key": NaN}'::json5::json;        -- should ERROR

-- cast: json5 -> jsonb
SELECT '{"key": "value"}'::json5::jsonb;
SELECT $json5${key: 'value'}$json5$::json5::jsonb;
SELECT '{true: 1, null: 2}'::json5::jsonb;
SELECT '{"a": .5}'::json5::jsonb;
SELECT '{"a": 5.}'::json5::jsonb;
SELECT '{"a": 0xFF}'::json5::jsonb;
SELECT '[1, 2, 3,]'::json5::jsonb;
SELECT '[[1], [2], {"a": [3, {"b": 4}]}]'::json5::jsonb;
SELECT '{a: 1, a: 2}'::json5::jsonb;
SELECT '{"key": Infinity}'::json5::jsonb;  -- should ERROR
SELECT '{"key": -Infinity}'::json5::jsonb; -- should ERROR
SELECT '{"key": +Infinity}'::json5::jsonb; -- should ERROR
SELECT '{"key": NaN}'::json5::jsonb;       -- should ERROR

-- JSON-producing functions embed json5 values via the json5 -> json cast
SELECT to_json($j${a: 1, /* c */ b: [.5, 'x',]}$j$::json5);
SELECT to_jsonb($j${a: 1, /* c */ b: [.5, 'x',]}$j$::json5);
SELECT json_build_object('k', '{a: 1,}'::json5),
       jsonb_build_object('k', '{a: 1,}'::json5);
SELECT json_agg(v), jsonb_agg(v)
  FROM (VALUES ('{a: 1}'::json5), ('[2,]'::json5)) t(v);
SELECT row_to_json(t) FROM (SELECT 1 AS id, '{a: 1}'::json5 AS j) t;
SELECT JSON_OBJECT('k': '{a: 1}'::json5), JSON_ARRAY('[1,]'::json5);
SELECT to_json('[Infinity]'::json5);           -- should ERROR
SELECT json_build_object('{a: 1}'::json5, 1);  -- should ERROR, key not scalar
-- values that are already strict JSON take the cast's verbatim path
CREATE TABLE json5_plain(j json5);
INSERT INTO json5_plain VALUES ('{"a": 1}'), ('[1, 2]');
SELECT to_json(j), row_to_json(json5_plain) FROM json5_plain;
SELECT json_agg(j) FROM json5_plain;
SELECT to_json('{"a": 1}'::json5) FROM generate_series(1, 2);
DROP TABLE json5_plain;

-- cast: json -> json5 (implicit, binary coercible)
SELECT '{"key": "value"}'::json::json5;
-- so changing a json column to json5 needs no table rewrite
CREATE TABLE json5_alter(j json);
INSERT INTO json5_alter VALUES ('{"key": "value"}');
SELECT pg_relation_filenode('json5_alter') AS before_filenode \gset
ALTER TABLE json5_alter ALTER COLUMN j TYPE json5;
SELECT pg_relation_filenode('json5_alter') = :before_filenode AS no_rewrite;
SELECT j, pg_typeof(j) FROM json5_alter;
DROP TABLE json5_alter;

-- cast: jsonb -> json5 (assignment; serializes then validates)
SELECT '{"key": "value"}'::jsonb::json5;
SELECT '{"a": 0.5, "b": [1,2,3]}'::jsonb::json5;

-- cast: json5 -> text (automatic I/O coercion; verbatim)
SELECT '{"key": "value"}'::json5::text;
SELECT $json5${key: 'value', /* comment */}$json5$::json5::text;

-- cast: text -> json5 (automatic I/O coercion; validates)
SELECT CAST('{"key": "value"}' AS json5);
SELECT CAST($json5${key: 'value'}$json5$ AS json5);
SELECT 'not json5 {'::text::json5;         -- should ERROR (validated)

-- deep nesting: json5 -> json must not impose an arbitrary depth cap
-- (parity with json5 -> jsonb and plain json; 100 > old fixed limit of 64)
SELECT (repeat('[', 100) || '1' || repeat(']', 100))::json5::json IS NOT NULL AS to_json_ok,
       (repeat('[', 100) || '1' || repeat(']', 100))::json5::jsonb IS NOT NULL AS to_jsonb_ok,
       (repeat('[', 100) || '1' || repeat(']', 100))::json IS NOT NULL AS json_ok;

-- table storage test
CREATE TABLE json5_test(id serial, data json5);
INSERT INTO json5_test(data) VALUES ($json5${key: 'value', /* comment */}$json5$);
INSERT INTO json5_test(data) VALUES ('{"normal": "json"}');
INSERT INTO json5_test(data) VALUES ('[1, 2, 3,]');
SELECT * FROM json5_test;
SELECT id, data::jsonb FROM json5_test;
DROP TABLE json5_test;
