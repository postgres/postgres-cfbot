--
-- TPC-H Combined Query Stream Script
--
-- Can be run directly or alongside individual tpch_q*.sql scripts in a
-- sequential client group (--group name=tpch,sequential=true,file=...).
--

\set delta random(60, 120)
\set ship_cutoff int_to_date('1998-12-01', -:delta, 'day')
\set yr_off random(0, 4)
\set start_date int_to_date('1993-01-01', :yr_off, 'year')
\set end_date date_add(:start_date, 1, 'year')

SELECT
    l_returnflag,
    l_linestatus,
    sum(l_quantity) AS sum_qty,
    sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
    count(*) AS count_order
FROM
    lineitem
WHERE
    l_shipdate <= :ship_cutoff::date
GROUP BY
    l_returnflag,
    l_linestatus
ORDER BY
    l_returnflag,
    l_linestatus;

SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM
    lineitem
WHERE
    l_shipdate >= :start_date::date
    AND l_shipdate < :end_date::date;
