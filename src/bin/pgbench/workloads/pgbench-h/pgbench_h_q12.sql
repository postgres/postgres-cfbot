--
-- pgbench-h (TPC-H-inspired) Query 12: Shipping Modes and Order Priority Query
--

\set yr_off random(0, 4)
\set start_date int_to_date('1993-01-01', :yr_off, 'year')
\set end_date date_add(:start_date, 1, 'year')

SELECT
    l_shipmode,
    sum(CASE
        WHEN o_orderpriority = '1-URGENT'
             OR o_orderpriority = '2-HIGH'
        THEN 1
        ELSE 0
    END) AS high_line_count,
    sum(CASE
        WHEN o_orderpriority <> '1-URGENT'
             AND o_orderpriority <> '2-HIGH'
        THEN 1
        ELSE 0
    END) AS low_line_count
FROM
    orders,
    lineitem
WHERE
    o_orderkey = l_orderkey
    AND l_shipmode IN ('MAIL', 'SHIP')
    AND l_commitdate < l_receiptdate
    AND l_shipdate < l_commitdate
    AND l_receiptdate >= :start_date::date
    AND l_receiptdate < :end_date::date
GROUP BY
    l_shipmode
ORDER BY
    l_shipmode;
