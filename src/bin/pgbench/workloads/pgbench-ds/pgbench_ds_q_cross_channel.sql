--
-- TPC-DS Cross-Channel Revenue & Demographics Query (Q4 / Q11 style)
--
-- Joins store_sales, catalog_sales, and web_sales with customer and
-- customer_demographics using CTEs.
--

\set yr random(2000, 2001)

WITH channel_totals AS (
    SELECT
        'store' AS channel,
        ss.ss_customer_sk AS customer_sk,
        d.d_year AS sales_year,
        sum(ss.ss_ext_sales_price) AS sales_amt,
        sum(ss.ss_net_profit) AS net_profit
    FROM store_sales ss
    JOIN date_dim d ON ss.ss_sold_date_sk = d.d_date_sk
    WHERE d.d_year = :yr
    GROUP BY ss.ss_customer_sk, d.d_year
    UNION ALL
    SELECT
        'catalog' AS channel,
        cs.cs_bill_customer_sk AS customer_sk,
        d.d_year AS sales_year,
        sum(cs.cs_ext_sales_price) AS sales_amt,
        sum(cs.cs_net_profit) AS net_profit
    FROM catalog_sales cs
    JOIN date_dim d ON cs.cs_sold_date_sk = d.d_date_sk
    WHERE d.d_year = :yr
    GROUP BY cs.cs_bill_customer_sk, d.d_year
    UNION ALL
    SELECT
        'web' AS channel,
        ws.ws_bill_customer_sk AS customer_sk,
        d.d_year AS sales_year,
        sum(ws.ws_ext_sales_price) AS sales_amt,
        sum(ws.ws_net_profit) AS net_profit
    FROM web_sales ws
    JOIN date_dim d ON ws.ws_sold_date_sk = d.d_date_sk
    WHERE d.d_year = :yr
    GROUP BY ws.ws_bill_customer_sk, d.d_year
)
SELECT
    ct.channel,
    cd.cd_gender,
    cd.cd_education_status,
    count(DISTINCT ct.customer_sk) AS active_customers,
    sum(ct.sales_amt) AS channel_revenue,
    sum(ct.net_profit) AS channel_profit
FROM
    channel_totals ct
    JOIN customer c ON c.c_customer_sk = ct.customer_sk
    JOIN customer_demographics cd ON cd.cd_demo_sk = c.c_current_cdemo_sk
GROUP BY
    ct.channel,
    cd.cd_gender,
    cd.cd_education_status
ORDER BY
    channel_revenue DESC
LIMIT 20;
