--
-- CH-benCHmark Analytical Query 6: Forecasting Revenue Change Query
--

SELECT
    COALESCE(sum(ol_amount), 0) AS revenue
FROM
    order_line
WHERE
    ol_quantity BETWEEN 1 AND 15;
