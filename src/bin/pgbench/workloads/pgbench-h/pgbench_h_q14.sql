--
-- pgbench-h (TPC-H-inspired) Query 14: Promotion Effect Query
--

\set m_off random(0, 59)
\set start_date int_to_date('1993-01-01', :m_off, 'month')
\set end_date date_add(:start_date, 1, 'month')

SELECT
    100.00 * sum(CASE
        WHEN p_type LIKE 'PROMO%'
        THEN l_extendedprice * (1 - l_discount)
        ELSE 0
    END) / COALESCE(NULLIF(sum(l_extendedprice * (1 - l_discount)), 0), 1) AS promo_revenue
FROM
    lineitem,
    part
WHERE
    l_partkey = p_partkey
    AND l_shipdate >= :start_date::date
    AND l_shipdate < :end_date::date;
