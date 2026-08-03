-- Join MCV statistics tests

--
-- Note: tables for which we check estimated row counts should be created
-- with autovacuum_enabled = off, so that we don't have unstable results
-- from auto-analyze happening when we didn't expect it.
--

-- directory paths are passed to us in environment variables
\getenv abs_srcdir PG_ABS_SRCDIR

-- prepare some test data
CREATE TABLE keyword (
    id integer NOT NULL PRIMARY KEY,
    keyword text NOT NULL,
    phonetic_code character varying(5)
);

CREATE TABLE movie_keyword (
    id integer NOT NULL PRIMARY KEY,
    movie_id integer NOT NULL,
    keyword_id integer NOT NULL
);

\set keyword_filename :abs_srcdir '/data/keyword.csv'
COPY keyword FROM :'keyword_filename' DELIMITER ',' CSV NULL '' ESCAPE '\' HEADER;
\set movie_keyword_filename :abs_srcdir '/data/movie_keyword.csv'
COPY movie_keyword FROM :'movie_keyword_filename' DELIMITER ',' CSV NULL '' ESCAPE '\' HEADER;

CREATE INDEX keyword_id_movie_keyword ON movie_keyword(keyword_id);

SET default_statistics_target = 10000;
ANALYZE keyword;
ANALYZE movie_keyword;

-- w/o join MCV statistics, planner would use a nested loop join
EXPLAIN (verbose, costs off)
SELECT * FROM keyword k, movie_keyword mk WHERE k.keyword IN ('superhero',
                                                              'sequel',
                                                              'based-on-comic',
                                                              'fight',
                                                              'violence') AND k.id = mk.keyword_id;

-- Create join MCV statistics on keyword.keyword
CREATE STATISTICS movie_keyword_join_stats (mcv)
ON k.keyword
FROM movie_keyword mk JOIN keyword k ON (mk.keyword_id = k.id);
ANALYZE movie_keyword;

-- w/ join MCV statistics, planner would use a hash join
EXPLAIN (verbose,costs off)
SELECT * FROM keyword k, movie_keyword mk WHERE k.keyword IN ('superhero',
                                                              'sequel',
                                                              'based-on-comic',
                                                              'fight',
                                                              'violence') AND k.id = mk.keyword_id;
RESET default_statistics_target;
