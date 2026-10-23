--
-- pgbench-e (TPC-E-inspired) Customer-Position Transaction Script
--
-- Features:
--   - Captures customer's account ID array into :ca_ids via \gset
--   - Uses 0-based array subscripting :ca_ids[0]
--   - Captures execution metadata via \gset_meta
--

\set c_id random(1, 100 * :scale)
\set get_hist (random(1, 100) <= 50)

SELECT c_id AS cust_id,
       ARRAY(SELECT ca_id FROM customer_account WHERE ca_c_id = :c_id ORDER BY ca_id LIMIT 10) AS ca_ids
  FROM customer
 WHERE c_id = :c_id \gset

\set target_ca :ca_ids[0]
CALL pgbench_e_customer_position(:cust_id, :target_ca, :get_hist) \gset_meta cp_
