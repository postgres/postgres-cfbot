--
-- TPC-DS Store Sales & Customer Returns Analytical Query (Q1 / Q3 / Q7 style)
--

\set yr random(2000, 2001)
\set mfg_id random_gaussian(1, 15, 2.5)

WITH customer_total_return AS (
    SELECT
        sr_customer_sk AS ctr_customer_sk,
        sr_store_sk AS ctr_store_sk,
        sum(sr_return_amt) AS ctr_total_return
    FROM
        store_returns
        JOIN date_dim ON sr_returned_date_sk = d_date_sk
    WHERE
        d_year = :yr
    GROUP BY
        sr_customer_sk,
        sr_store_sk
)
SELECT
    i.i_brand_id,
    i.i_brand,
    i.i_category,
    count(DISTINCT ss.ss_ticket_number) AS tickets,
    sum(ss.ss_ext_sales_price) AS total_sales,
    sum(ss.ss_net_profit) AS total_profit,
    COALESCE(max(ctr.ctr_total_return), 0) AS max_store_return
FROM
    date_dim dt
    JOIN store_sales ss ON dt.d_date_sk = ss.ss_sold_date_sk
    JOIN item i ON ss.ss_item_sk = i.i_item_sk
    LEFT JOIN customer_total_return ctr
           ON ctr.ctr_customer_sk = ss.ss_customer_sk
          AND ctr.ctr_store_sk = ss.ss_store_sk
WHERE
    dt.d_year = :yr
    AND (i.i_manufact_id = :mfg_id OR i.i_category_id BETWEEN 1 AND 6)
GROUP BY
    i.i_brand_id,
    i.i_brand,
    i.i_category
ORDER BY
    total_sales DESC,
    i.i_brand_id
LIMIT 20;
