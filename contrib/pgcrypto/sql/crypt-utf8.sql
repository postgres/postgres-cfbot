/* Needs UTF8; skip otherwise (truncated multibyte salt copy). */
SELECT getdatabaseencoding() <> 'UTF8' AS skip_test \gset
\if :skip_test
\quit
\endif
-- Salt bytes e282ac41 (euro then A). DES copies only e282, which is invalid UTF8.
SELECT crypt('password', convert_from(decode('e282ac41', 'hex'), 'utf8'));
