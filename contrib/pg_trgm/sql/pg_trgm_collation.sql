/*
 * This test is for ICU collations.
 */

/* skip test if not UTF8 server encoding or no ICU collations installed */
SELECT getdatabaseencoding() <> 'UTF8' OR
       (SELECT count(*) FROM pg_collation WHERE collprovider = 'i' AND collname <> 'unicode') = 0
       AS skip_test \gset
\if :skip_test
\quit
\endif

-- Test that lowercase conversion of trigrams uses specified collation

CREATE TABLE test(col TEXT COLLATE "tr-x-icu");
INSERT INTO test VALUES ('ISTANBUL');
SELECT show_trgm(col) FROM test;
SELECT show_trgm('ISTANBUL' COLLATE "tr-x-icu");

SELECT show_trgm('ISTANBUL' COLLATE "C");

SELECT similarity('ıstanbul' COLLATE "tr-x-icu", 'ISTANBUL' COLLATE "tr-x-icu");
SELECT similarity('ıstanbul' COLLATE "C", 'ISTANBUL' COLLATE "C");

