--
-- pgbench-h (TPC-H-inspired) Query 10: Returned Item Reporting Query
--

\set m_off random(0, 23)
\set start_date int_to_date('1993-02-01', :m_off, 'month')
\set end_date date_add(:start_date, 3, 'month')

SELECT
    c_custkey,
    c_name,
    sum(l_extendedprice * (1 - l_discount)) AS revenue,
    c_acctbal,
    n_name,
    c_address,
    c_phone,
    c_comment
FROM
    customer,
    orders,
    lineitem,
    nation
WHERE
    c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND o_orderdate >= :start_date::date
    AND o_orderdate < :end_date::date
    AND l_returnflag = 'R'
    AND c_nationkey = n_nationkey
GROUP BY
    c_custkey,
    c_name,
    c_acctbal,
    c_phone,
    n_name,
    c_address,
    c_comment
ORDER BY
    revenue DESC
LIMIT 20;
