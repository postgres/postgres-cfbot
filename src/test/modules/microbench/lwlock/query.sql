-- lwlock micro-benchmark query
--
-- :rounds and :iterations are psql variables (run-test.sh sets them).
-- They are not expanded inside dollar-quoted strings.  \gexec runs one
-- SELECT per n_parallel so each result set prints as soon as that size
-- finishes.

\pset format aligned

\if :{?rounds}
\else
\set rounds 1000
\endif
\if :{?iterations}
\else
\set iterations 128
\endif


\timing on

SELECT format($q$
SELECT %s AS n_parallel,  bench_stats.*
FROM format_microbench_with_count(
	(SELECT array_agg(s ORDER BY s.id)
	 FROM bench_lwlock(%s, %s::int8, %s::int8) AS s)
) bench_stats;
$q$, i, i, :rounds, :iterations)
FROM generate_series(1, 4) AS i
\gexec
