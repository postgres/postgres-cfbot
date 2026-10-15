--
-- pgbench-h (TPC-H-inspired) Query 5: Local Supplier Volume Query
--

\set reg_idx random(0, 4)
\set r_name CASE WHEN :reg_idx = 0 THEN 'AFRICA' WHEN :reg_idx = 1 THEN 'AMERICA' WHEN :reg_idx = 2 THEN 'ASIA' WHEN :reg_idx = 3 THEN 'EUROPE' ELSE 'MIDDLE EAST' END
\set yr_off random(0, 4)
\set start_date int_to_date('1993-01-01', :yr_off, 'year')
\set end_date date_add(:start_date, 1, 'year')

SELECT
    n_name,
    sum(l_extendedprice * (1 - l_discount)) AS revenue
FROM
    customer,
    orders,
    lineitem,
    supplier,
    nation,
    region
WHERE
    c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND l_suppkey = s_suppkey
    AND c_nationkey = s_nationkey
    AND s_nationkey = n_nationkey
    AND n_regionkey = r_regionkey
    AND r_name = :r_name
    AND o_orderdate >= :start_date::date
    AND o_orderdate < :end_date::date
GROUP BY
    n_name
ORDER BY
    revenue DESC;
