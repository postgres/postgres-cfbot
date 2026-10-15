--
-- TPC-DS Post-Load Secondary Indexes and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_store_sales_sold_date
    ON store_sales (ss_sold_date_sk, ss_store_sk);

CREATE INDEX IF NOT EXISTS idx_store_returns_returned_date
    ON store_returns (sr_returned_date_sk, sr_store_sk);

CREATE INDEX IF NOT EXISTS idx_catalog_sales_sold_date
    ON catalog_sales (cs_sold_date_sk);

CREATE INDEX IF NOT EXISTS idx_web_sales_sold_date
    ON web_sales (ws_sold_date_sk);

CREATE INDEX IF NOT EXISTS idx_inventory_item_date
    ON inventory (inv_item_sk, inv_date_sk);

ANALYZE date_dim, item, customer_address, customer_demographics, customer,
        store, warehouse, promotion, store_sales, store_returns,
        catalog_sales, web_sales, inventory;
