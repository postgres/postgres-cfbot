-- bufmap micro-benchmark query
--
-- 1 row = 1 buffer: pad is STORAGE PLAIN and larger than half a page.
-- Grow anchor to 90% of shared_buffers if needed.  iterations stays the
-- runner value so n_parallel * iterations fits in that heap.

\pset format aligned

\if :{?rounds}
\else
\set rounds 1000
\endif
\if :{?iterations}
\else
\set iterations 128
\endif
\if :{?max_parallel}
\else
\set max_parallel 10
\endif

\timing on

SELECT format($q$
SELECT %s AS workers,
op || coalesce(' / ' || "group"::text, '') as "op / wait"
, avg, q1, med, q3, count
FROM format_microbench_with_count(
	(SELECT array_agg(s ORDER BY s.id)s
	 FROM bench_bufmap(%s, %s::int8, (%s / %s)::int8) AS s)
) bench_stats
ORDER BY 1,2;
$q$, i, i, :rounds, :iterations, i)
FROM generate_series(1, :max_parallel) AS i
\gexec
