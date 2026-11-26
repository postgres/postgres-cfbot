-- interval check

CREATE TABLE intervaltmp (a interval);

\copy intervaltmp from 'data/interval.data'

SET enable_seqscan=on;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23';

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23';

SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;

CREATE INDEX intervalidx ON intervaltmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM intervaltmp WHERE a <  '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a <= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a  = '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >= '199 days 21:21:23'::interval;

SELECT count(*) FROM intervaltmp WHERE a >  '199 days 21:21:23'::interval;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;

SET enable_indexonlyscan=off;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;
SELECT a, a <-> '199 days 21:21:23' FROM intervaltmp ORDER BY a <-> '199 days 21:21:23' LIMIT 3;

-- infinite intervals (the <-> operator must not report overflow)
CREATE TABLE intervalinf (a interval);
CREATE INDEX intervalinfidx ON intervalinf USING gist (a);
INSERT INTO intervalinf SELECT make_interval(days => g) FROM generate_series(1, 300) g;
INSERT INTO intervalinf VALUES ('infinity'), ('infinity'), ('-infinity'), ('-infinity');
SELECT 'infinity'::interval <-> 'infinity'::interval AS same_pos,
       '-infinity'::interval <-> '-infinity'::interval AS same_neg,
       'infinity'::interval <-> '-infinity'::interval AS opposite,
       '1 day'::interval <-> 'infinity'::interval AS finite_inf;
EXPLAIN (COSTS OFF)
SELECT a, a <-> 'infinity' FROM intervalinf ORDER BY a <-> 'infinity' LIMIT 3;
SELECT a, a <-> 'infinity' FROM intervalinf ORDER BY a <-> 'infinity' LIMIT 3;
SELECT a, a <-> '-infinity' FROM intervalinf ORDER BY a <-> '-infinity' LIMIT 3;
