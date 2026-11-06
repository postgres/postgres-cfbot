-- timestamp check

CREATE TABLE timestamptmp (a timestamp);

\copy timestamptmp from 'data/timestamp.data'

SET enable_seqscan=on;

SELECT count(*) FROM timestamptmp WHERE a <  '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a <= '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a  = '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a >= '2004-10-26 08:55:08';

SELECT count(*) FROM timestamptmp WHERE a >  '2004-10-26 08:55:08';

SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;

CREATE INDEX timestampidx ON timestamptmp USING gist ( a );

SET enable_seqscan=off;

SELECT count(*) FROM timestamptmp WHERE a <  '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a <= '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a  = '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a >= '2004-10-26 08:55:08'::timestamp;

SELECT count(*) FROM timestamptmp WHERE a >  '2004-10-26 08:55:08'::timestamp;

EXPLAIN (COSTS OFF)
SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;
SELECT a, a <-> '2004-10-26 08:55:08' FROM timestamptmp ORDER BY a <-> '2004-10-26 08:55:08' LIMIT 3;

-- An ordering scan that hands an exact point distance to the target list
-- while another ORDER BY key, here an interval, is of a by-reference type.
CREATE TABLE timestamppt (p point, a timestamp);
INSERT INTO timestamppt
  SELECT point(i % 7, i % 11), '2004-10-26 08:55:08'::timestamp + i * interval '37 min'
  FROM generate_series(1, 600) i;
CREATE INDEX timestamppt_idx ON timestamppt USING gist (p, a);
SET enable_indexonlyscan=off;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT p <-> point(3, 4) AS d, a <-> '2004-10-26 08:55:08' AS da
  FROM timestamppt
  ORDER BY p <-> point(3, 4), a <-> '2004-10-26 08:55:08' LIMIT 3;
SELECT p <-> point(3, 4) AS d, a <-> '2004-10-26 08:55:08' AS da
  FROM timestamppt
  ORDER BY p <-> point(3, 4), a <-> '2004-10-26 08:55:08' LIMIT 3;
RESET enable_indexonlyscan;
DROP TABLE timestamppt;
