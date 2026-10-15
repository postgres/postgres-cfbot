--
-- pgbench-h (TPC-H-inspired) Query 6: Forecasting Revenue Change Query
--

\set yr_off random(0, 4)
\set start_date int_to_date('1993-01-01', :yr_off, 'year')
\set end_date date_add(:start_date, 1, 'year')
\set disc_int random(2, 9)
\set disc_lo (:disc_int - 1) / 100.0
\set disc_hi (:disc_int + 1) / 100.0
\set qty random(24, 25)

SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM
    lineitem
WHERE
    l_shipdate >= :start_date::date
    AND l_shipdate < :end_date::date
    AND l_discount BETWEEN :disc_lo AND :disc_hi
    AND l_quantity < :qty;
