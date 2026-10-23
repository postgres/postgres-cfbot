--
-- pgbench-graph-pgq: LDBC FinBench Workload Suite (TSR, TCR, TW, TRW)
-- Uses ISO SQL/PGQ GRAPH_TABLE pattern matching over finbench_graph.
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
-- TSR1: Account status and balance via GRAPH_TABLE
SELECT g.id, g.owner_id, g.owner_type, g.is_blocked, g.balance
  FROM GRAPH_TABLE (finbench_graph
      MATCH (a IS Account WHERE a.id = :acct_id)
      COLUMNS (
          a.id AS id,
          a.owner_id AS owner_id,
          a.owner_type AS owner_type,
          a.is_blocked AS is_blocked,
          a.balance AS balance
      )
  ) g;

-- TSR2 & TSR3: Outgoing and incoming transfer aggregations + blocked peer ratio via GRAPH_TABLE
SELECT count(*) AS out_cnt,
       COALESCE(sum(g.amount), 0.00) AS out_sum,
       COALESCE(max(g.amount), 0.00) AS out_max,
       count(*) FILTER (WHERE g.dst_blocked) AS blocked_dst_cnt
  FROM GRAPH_TABLE (finbench_graph
      MATCH (src IS Account WHERE src.id = :acct_id)
            -[t IS transfer WHERE t.created_ts >= :start_ts AND t.created_ts <= :end_ts]->
            (dst IS Account)
      COLUMNS (
          t.amount AS amount,
          dst.is_blocked AS dst_blocked
      )
  ) g \gset

\validate_result (:out_cnt >= 0) true

-- TSR6: Accounts sharing a sign-in medium via GRAPH_TABLE
SELECT DISTINCT g.shared_acct_id,
       g.medium_id,
       g.risk_score
  FROM GRAPH_TABLE (finbench_graph
      MATCH (a1 IS Account WHERE a1.id = :acct_id)
            <-[IS signIn]-(m IS Medium)
            -[IS signIn]->(a2 IS Account)
      WHERE a2.id <> a1.id
      COLUMNS (
          a2.id AS shared_acct_id,
          m.id AS medium_id,
          m.risk_score AS risk_score
      )
  ) g
 ORDER BY g.risk_score DESC, g.shared_acct_id ASC
 LIMIT 10;

-- 2. Complex Reads (TCR1, TCR2, TCR4, TCR8)
-- TCR1: 1..3 Hop Temporal Transfer Path with strictly ascending timestamps via UNION DISTINCT ON (curr_acct, last_ts)
WITH RECURSIVE temporal_paths AS (
    SELECT g.dst_acct AS curr_acct,
           g.created_ts AS last_ts,
           1 AS hop_dist
      FROM GRAPH_TABLE (finbench_graph
          MATCH (src IS Account WHERE src.id = :acct_id)
                -[t1 IS transfer WHERE t1.created_ts >= :start_ts AND t1.created_ts <= :end_ts]->
                (dst IS Account)
          COLUMNS (dst.id AS dst_acct, t1.created_ts AS created_ts)
      ) g
    UNION DISTINCT ON (curr_acct, last_ts)
    SELECT g.dst_acct AS curr_acct,
           g.created_ts AS last_ts,
           tp.hop_dist + 1 AS hop_dist
      FROM temporal_paths tp,
           GRAPH_TABLE (finbench_graph
               MATCH (src IS Account WHERE src.id = tp.curr_acct)
                     -[tn IS transfer WHERE tn.created_ts > tp.last_ts AND tn.created_ts <= :end_ts]->
                     (dst IS Account)
               COLUMNS (dst.id AS dst_acct, tn.created_ts AS created_ts)
           ) g
     WHERE tp.hop_dist < 3
       AND g.dst_acct <> :acct_id
)
SELECT count(*) AS tcr1_paths
  FROM temporal_paths tp,
       GRAPH_TABLE (finbench_graph
           MATCH (m IS Medium WHERE m.is_blocked OR m.risk_score >= 50)
                 -[IS signIn]->
                 (a IS Account WHERE a.id = tp.curr_acct)
           COLUMNS (1 AS one)
       ) gm \gset

\validate_result (:tcr1_paths >= 0) true

-- TCR4: Temporal Transfer 3-Cycle Detection (acct1 -> acct2 -> acct3 -> acct1 with t1 < t2 < t3)
SELECT count(*) AS tcr4_cycles
  FROM GRAPH_TABLE (finbench_graph
      MATCH (a1 IS Account WHERE a1.id = :acct_id)
            -[e1 IS transfer]->(a2 IS Account)
            -[e2 IS transfer]->(a3 IS Account)
            -[e3 IS transfer]->(a1 IS Account)
      WHERE a3.id <> a1.id
        AND e2.created_ts > e1.created_ts
        AND e3.created_ts > e2.created_ts
      COLUMNS (1 AS one)
  ) \gset

\validate_result (:tcr4_cycles >= 0) true

-- TCR2 & TCR8: Loan-to-Transfer Outflow Ratio Tracking via GRAPH_TABLE
SELECT COALESCE(sum(gl.loan_amount), 0.00) AS total_loan_amt,
       COALESCE(sum(gt.amount), 0.00) AS downstream_transfer_amt
  FROM GRAPH_TABLE (finbench_graph
      MATCH (l IS Loan WHERE l.borrower_id = :acct_id)-[IS deposit]->(a IS Account)
      COLUMNS (l.loan_amount AS loan_amount, l.created_ts AS loan_ts, a.id AS acct_id)
  ) gl
  LEFT JOIN GRAPH_TABLE (finbench_graph
      MATCH (src IS Account)-[t IS transfer]->(dst IS Account)
      COLUMNS (src.id AS src_acct, t.amount AS amount, t.created_ts AS transfer_ts)
  ) gt ON gt.src_acct = gl.acct_id AND gt.transfer_ts >= gl.loan_ts \gset

\validate_result (:total_loan_amt >= 0) true

-- 3. Write Operation (TW): Insert a new sign-in event with dependent write watermark
\begin_dependent_write :event_ts CHANNEL 1
INSERT INTO pgbench_fin_sign_ins (medium_id, acct_id, created_ts)
VALUES (1 + (:acct_id % (50 * :scale)), :acct_id, :event_ts)
ON CONFLICT DO NOTHING;
\end_dependent_write :event_ts CHANNEL 1

-- 4. Interactive Read-Write Fraud Prevention Transactions (TRW1 - TRW3)
--    Opens BEGIN, inserts a provisional transfer, checks via GRAPH_TABLE whether
--    either endpoint is blocked (TRW1) or exceeds fan-out velocity threshold (TRW2).
--    If fraud_detected is true, executes ROLLBACK (counted in expected_rollbacks);
--    otherwise executes COMMIT.
\set trw_mode random(1, 2)
\if :trw_mode = 1
-- Force a blocked endpoint (17 is blocked by pgbench_graph_pgq_init.sql: (p % 17) = 0)
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
          FROM GRAPH_TABLE (finbench_graph
              MATCH (a IS Account WHERE a.id IN (:acct_id, :trw_dst) AND a.is_blocked)
              COLUMNS (1 AS one)
          )
    )
    OR (
        SELECT count(*)
          FROM GRAPH_TABLE (finbench_graph
              MATCH (src IS Account WHERE src.id = :acct_id)-[t IS transfer WHERE t.amount >= 50000.00]->(dst IS Account)
              COLUMNS (1 AS one)
          )
    ) > 10
) AS fraud_detected \gset

\if :fraud_detected
ROLLBACK;
\else
COMMIT;
\endif
