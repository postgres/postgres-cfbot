--
-- pgbench-h (TPC-H-inspired) Query 3: Shipping Priority Query
--

\set seg_idx random(0, 4)
\set segment CASE WHEN :seg_idx = 0 THEN 'AUTOMOBILE' WHEN :seg_idx = 1 THEN 'BUILDING' WHEN :seg_idx = 2 THEN 'FURNITURE' WHEN :seg_idx = 3 THEN 'MACHINERY' ELSE 'HOUSEHOLD' END
\set day_off random(0, 30)
\set cutoff_date int_to_date('1995-03-01', :day_off, 'day')

SELECT
    l_orderkey,
    sum(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM
    customer,
    orders,
    lineitem
WHERE
    c_mktsegment = :segment
    AND c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND o_orderdate < :cutoff_date::date
    AND l_shipdate > :cutoff_date::date
GROUP BY
    l_orderkey,
    o_orderdate,
    o_shippriority
ORDER BY
    revenue DESC,
    o_orderdate
LIMIT 10;
