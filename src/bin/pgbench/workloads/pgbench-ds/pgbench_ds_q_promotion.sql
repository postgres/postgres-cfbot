--
-- TPC-DS Promotional Channel Effectiveness Query (Q19 / Q61 style)
--

\set yr random(2000, 2001)

SELECT
    i.i_category,
    sum(CASE WHEN p.p_channel_dmail = 'Y' OR p.p_channel_email = 'Y' OR p.p_channel_tv = 'Y'
             THEN ss.ss_ext_sales_price ELSE 0 END) AS promotional_sales,
    sum(ss.ss_ext_sales_price) AS total_sales,
    round(
        100.0 * sum(CASE WHEN p.p_channel_dmail = 'Y' OR p.p_channel_email = 'Y' OR p.p_channel_tv = 'Y'
                         THEN ss.ss_ext_sales_price ELSE 0 END)
              / COALESCE(NULLIF(sum(ss.ss_ext_sales_price), 0), 1),
        2
    ) AS promo_pct
FROM
    store_sales ss
    JOIN store s ON s.s_store_sk = ss.ss_store_sk
    JOIN promotion p ON p.p_promo_sk = ss.ss_promo_sk
    JOIN date_dim d ON d.d_date_sk = ss.ss_sold_date_sk
    JOIN customer c ON c.c_customer_sk = ss.ss_customer_sk
    JOIN customer_address ca ON ca.ca_address_sk = c.c_current_addr_sk
    JOIN item i ON i.i_item_sk = ss.ss_item_sk
WHERE
    d.d_year = :yr
GROUP BY
    i.i_category
ORDER BY
    total_sales DESC;
