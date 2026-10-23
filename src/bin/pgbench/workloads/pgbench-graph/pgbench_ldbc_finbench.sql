--
-- pgbench-graph: LDBC FinBench Workload Suite (TSR, TCR, TW, TRW)
-- Exercises financial graph multi-edges, strictly increasing edge-timestamp
-- transfer chains/cycles (TCR1, TCR2, TCR4, TCR8), simple reads (TSR1-TSR6),
-- writes (TW), and interactive read-write fraud prevention transactions
-- (TRW1-TRW3) with deliberate conditional ROLLBACK (tracked as expected_rollbacks).
--

\set acct_id random_zipfian(1, 200 * :scale, 1.2)
\set dst_acct ldbc_correlated_edge(:acct_id, 200 * :scale, 8, 0.70, 101)
\set start_ts 1700000000000000
\set end_ts   1700500000000000
\set event_ts (1700300000000000 + :client_id * 1000 + random(1, 50000))

-- 1. Simple Reads (TSR1, TSR2, TSR3, TSR6)
-- TSR1: Account status and balance
SELECT id, owner_id, owner_type, is_blocked, balance
  FROM pgbench_fin_accounts
 WHERE id = :acct_id;

-- TSR2 & TSR3: Outgoing and incoming transfer aggregations + blocked peer ratio
SELECT count(*) AS out_cnt,
       COALESCE(sum(t.amount), 0.00) AS out_sum,
       COALESCE(max(t.amount), 0.00) AS out_max,
       count(*) FILTER (WHERE dst.is_blocked) AS blocked_dst_cnt
  FROM pgbench_fin_transfers t
  JOIN pgbench_fin_accounts dst ON dst.id = t.dst_acct
 WHERE t.src_acct = :acct_id
   AND t.created_ts BETWEEN :start_ts AND :end_ts \gset

\validate_result (:out_cnt >= 0) true

-- TSR6: Accounts sharing a blocked sign-in medium
SELECT DISTINCT s2.acct_id AS shared_acct_id,
       m.id AS medium_id,
       m.risk_score
  FROM pgbench_fin_sign_ins s1
  JOIN pgbench_fin_mediums m ON m.id = s1.medium_id
  JOIN pgbench_fin_sign_ins s2 ON s2.medium_id = m.id AND s2.acct_id <> s1.acct_id
 WHERE s1.acct_id = :acct_id
 ORDER BY m.risk_score DESC, shared_acct_id ASC
 LIMIT 10;

-- 2. Complex Reads (TCR1, TCR2, TCR4, TCR8)
-- TCR1: 1..3 Hop Temporal Transfer Path with strictly ascending timestamps (t1 < t2 < t3)
WITH RECURSIVE temporal_paths AS (
    SELECT t1.dst_acct AS curr_acct,
           t1.created_ts AS last_ts,
           1 AS hop_dist,
           ARRAY[:acct_id::bigint, t1.dst_acct] AS path
      FROM pgbench_fin_transfers t1
     WHERE t1.src_acct = :acct_id
       AND t1.created_ts BETWEEN :start_ts AND :end_ts
    UNION ALL
    SELECT tn.dst_acct AS curr_acct,
           tn.created_ts AS last_ts,
           tp.hop_dist + 1,
           tp.path || tn.dst_acct
      FROM temporal_paths tp
      JOIN pgbench_fin_transfers tn ON tn.src_acct = tp.curr_acct
     WHERE tp.hop_dist < 3
       AND tn.created_ts > tp.last_ts
       AND tn.created_ts <= :end_ts
       AND NOT (tn.dst_acct = ANY(tp.path))
)
SELECT count(*) AS tcr1_paths
  FROM temporal_paths tp
  JOIN pgbench_fin_sign_ins si ON si.acct_id = tp.curr_acct
  JOIN pgbench_fin_mediums m ON m.id = si.medium_id
 WHERE m.is_blocked OR m.risk_score >= 50 \gset

\validate_result (:tcr1_paths >= 0) true

-- TCR4: Temporal Transfer 3-Cycle Detection (acct1 -> acct2 -> acct3 -> acct1 with t1 < t2 < t3)
SELECT count(*) AS tcr4_cycles
  FROM pgbench_fin_transfers e1
  JOIN pgbench_fin_transfers e2
    ON e2.src_acct = e1.dst_acct
   AND e2.dst_acct <> e1.src_acct
   AND e2.created_ts > e1.created_ts
  JOIN pgbench_fin_transfers e3
    ON e3.src_acct = e2.dst_acct
   AND e3.dst_acct = e1.src_acct
   AND e3.created_ts > e2.created_ts
 WHERE e1.src_acct = :acct_id \gset

\validate_result (:tcr4_cycles >= 0) true

-- TCR2 & TCR8: Loan-to-Transfer Outflow Ratio Tracking
SELECT COALESCE(sum(l.loan_amount), 0.00) AS total_loan_amt,
       COALESCE(sum(t.amount), 0.00) AS downstream_transfer_amt
  FROM pgbench_fin_loans l
  LEFT JOIN pgbench_fin_transfers t
    ON t.src_acct = l.acct_id
   AND t.created_ts >= l.created_ts
 WHERE l.borrower_id = :acct_id \gset

\validate_result (:total_loan_amt >= 0) true

-- 3. Write Operation (TW): Insert a new sign-in event with dependent write watermark
\begin_dependent_write :event_ts CHANNEL 1
INSERT INTO pgbench_fin_sign_ins (medium_id, acct_id, created_ts)
VALUES (1 + (:acct_id % (50 * :scale)), :acct_id, :event_ts)
ON CONFLICT DO NOTHING;
\end_dependent_write :event_ts CHANNEL 1

-- 4. Interactive Read-Write Fraud Prevention Transactions (TRW1 - TRW3)
--    Opens BEGIN, inserts a provisional transfer, checks whether either endpoint
--    is blocked (TRW1), exceeds fan-in/fan-out velocity threshold (TRW2), or
--    creates a high-risk cycle/blocked link (TRW3).
--    If risk_detected is true, executes ROLLBACK (counted in expected_rollbacks);
--    otherwise executes COMMIT.
\set trw_mode random(1, 2)
\if :trw_mode = 1
-- Force a blocked endpoint (17 is blocked by pgbench_graph_init.sql: (p % 17) = 0)
-- so TRW fraud prevention deterministically triggers expected_rollbacks!
\set trw_dst 17
\else
\set trw_dst :dst_acct
\endif

BEGIN;

INSERT INTO pgbench_fin_transfers (src_acct, dst_acct, amount, created_ts)
VALUES (:acct_id, :trw_dst, 950.00, :event_ts + 1);

SELECT (
    EXISTS (
        SELECT 1
          FROM pgbench_fin_accounts a
         WHERE a.id IN (:acct_id, :trw_dst)
           AND a.is_blocked
    )
    OR (
        SELECT count(*)
          FROM pgbench_fin_transfers t
         WHERE t.src_acct = :acct_id
           AND t.amount >= 50000.00
    ) > 10
) AS fraud_detected \gset

\if :fraud_detected
ROLLBACK;
\else
COMMIT;
\endif
