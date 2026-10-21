--
-- pgbench-graph (Unified LDBC Social Network & FinBench Graph Workload Suite)
-- Parallel In-Database Data Generation Script
--
-- Usage:
--   pgbench -n -c <N> -j <N> -t 1 -D num_workers=<N> -s <scale> \
--           -f workloads/pgbench-graph/pgbench_graph_init.sql
--

SELECT
    COALESCE(substring(':num_workers' FROM '^[0-9]+$')::int, 1) AS num_workers,
    COALESCE(substring(':persons_per_scale' FROM '^[0-9]+$')::int, 200) AS persons_per_scale
\gset

SELECT pgbench.setseed((:random_seed + :client_id)::bigint);

-- 1. Places (continents 1..6, countries 7..16, cities 17..66; partitioned across workers)
INSERT INTO pgbench_graph_places (id, name, type, part_of_id)
SELECT pl,
       CASE
           WHEN pl <= 6 THEN pgbench.format_string('Continent_%d', pl)
           WHEN pl <= 16 THEN pgbench.format_string('Country_%d', pl)
           ELSE pgbench.format_string('City_%d', pl)
       END,
       CASE
           WHEN pl <= 6 THEN 'continent'
           WHEN pl <= 16 THEN 'country'
           ELSE 'city'
       END,
       CASE
           WHEN pl <= 6 THEN NULL
           WHEN pl <= 16 THEN 1 + ((pl - 7) % 6)
           ELSE 7 + ((pl - 17) % 10)
       END
  FROM generate_series(:client_id + 1, 66, :num_workers) AS pl;

-- 2. Tags (1..100 across 10 tagclasses; partitioned across workers)
INSERT INTO pgbench_graph_tags (id, name, tagclass_id)
SELECT t,
       pgbench.format_string('Tag_%d', t),
       1 + ((t - 1) % 10)
  FROM generate_series(:client_id + 1, 100, :num_workers) AS t;

-- 3. Persons (partitioned across workers; 200 * :scale persons by default)
INSERT INTO pgbench_graph_persons (
    id, first_name, last_name, gender, city_id, company_id, university_id, created_ts
)
SELECT p,
       pgbench.format_string('First%d', p),
       pgbench.c_last((p % 1000)::int),
       CASE WHEN (p % 2) = 0 THEN 'female' ELSE 'male' END,
       17 + (p % 50)::int,
       1 + (p % 20)::int,
       1 + (p % 15)::int,
       1700000000000000::bigint + p * 1000
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p;

-- 4. Person Interest Tags (2 tags per person)
INSERT INTO pgbench_graph_person_tags (person_id, tag_id)
SELECT p,
       1 + ((p * 7 + k * 13) % 100)::int
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 2) AS k;

-- 5. Bidirectional knows edges using LDBC homophily + power-law degree generator
INSERT INTO pgbench_graph_knows (src_id, dst_id, weight, created_ts)
SELECT DISTINCT
       edges.a,
       edges.b,
       round((0.1 + ((edges.a + edges.b) % 10) * 0.1)::numeric, 2)::float8,
       1700000000000000::bigint + LEAST(edges.a, edges.b) * 1000 + GREATEST(edges.a, edges.b)
  FROM (
      SELECT p AS src,
             pgbench.ldbc_correlated_edge(
                 p,
                 (:persons_per_scale * :scale)::bigint,
                 10::bigint,
                 0.75::float8,
                 (42 + k)::bigint
             ) AS dst
        FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
        CROSS JOIN generate_series(1, 3) AS k
  ) AS raw_pairs
  CROSS JOIN LATERAL (
      VALUES (raw_pairs.src, raw_pairs.dst),
             (raw_pairs.dst, raw_pairs.src)
  ) AS edges(a, b)
 WHERE raw_pairs.src <> raw_pairs.dst;

-- 6. Forums (20 * :scale forums) & Forum Memberships
INSERT INTO pgbench_graph_forums (id, title, moderator_id, created_ts)
SELECT f,
       pgbench.format_string('Forum_%d_Topic_%d', f, 1 + (f % 10)),
       1 + ((f * 7) % (:persons_per_scale * :scale)),
       1700000000000000::bigint + f * 500
  FROM generate_series(:client_id + 1, 20 * :scale, :num_workers) AS f;

INSERT INTO pgbench_graph_forum_members (forum_id, person_id, joined_ts)
SELECT 1 + ((p + m * 3) % (20 * :scale)),
       p,
       1700000000000000::bigint + p * 1200 + m * 100
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 2) AS m;

-- 7. Messages: Top-level Posts (id = (p-1)*2 + 1, parent_message_id IS NULL)
--    and Comment Replies (id = (p-1)*2 + 2, parent_message_id = prior Post)
--    with LDBC flashmob timestamp spikes
INSERT INTO pgbench_graph_messages (
    id, creator_id, forum_id, parent_message_id, country_id, content, length, created_ts
)
SELECT msg_id,
       creator_id,
       forum_id,
       parent_msg_id,
       country_id,
       content_txt,
       length(content_txt),
       created_ts
  FROM (
      SELECT (p - 1) * 2 + m AS msg_id,
             p AS creator_id,
             1 + ((p + m) % (20 * :scale)) AS forum_id,
             CASE
                 WHEN m = 1 THEN NULL::bigint
                 ELSE (1 + ((p + 3) % (:persons_per_scale * :scale)) - 1) * 2 + 1
             END AS parent_msg_id,
             7 + ((p + m) % 10)::int AS country_id,
             CASE
                 WHEN m = 1 THEN pgbench.format_string('Post #%d by person %d: %s', m, p, pgbench.random_astring(16, 48))
                 ELSE pgbench.format_string('Reply #%d by person %d: %s', m, p, pgbench.random_astring(12, 32))
             END AS content_txt,
             pgbench.ldbc_flashmob_ts(
                 1700000000000000::bigint,
                 86400000000::bigint,
                 6::bigint,
                 0.45::float8,
                 (p * 2 + m)::bigint
             ) AS created_ts
        FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
        CROSS JOIN generate_series(1, 2) AS m
  ) AS gen_msgs;

-- 8. Likes & Message Tags
INSERT INTO pgbench_graph_likes (person_id, message_id, created_ts)
SELECT p,
       1 + ((p * 5 + l * 11) % (:persons_per_scale * :scale * 2)),
       1700010000000000::bigint + p * 1000 + l * 100
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 2) AS l;

INSERT INTO pgbench_graph_message_tags (message_id, tag_id)
SELECT (p - 1) * 2 + m,
       1 + (((p - 1) * 2 + m * 7 + t * 19) % 100)::int
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 2) AS m
  CROSS JOIN generate_series(1, 2) AS t;

-- 9. LDBC FinBench Financial Graph Entities & Multi-Edges
INSERT INTO pgbench_fin_accounts (id, owner_id, owner_type, is_blocked, balance, created_ts)
SELECT p,
       p,
       CASE WHEN (p % 10) = 0 THEN 'company' ELSE 'person' END,
       ((p % 17) = 0),
       round((5000.00 + (p % 50) * 250.00)::numeric, 2),
       1700000000000000::bigint + p * 500
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p;

INSERT INTO pgbench_fin_mediums (id, type, is_blocked, risk_score)
SELECT m,
       CASE (m % 3) WHEN 0 THEN 'card' WHEN 1 THEN 'mobile' ELSE 'web' END,
       ((m % 13) = 0),
       10 + ((m * 7) % 90)::int
  FROM generate_series(:client_id + 1, 50 * :scale, :num_workers) AS m;

INSERT INTO pgbench_fin_sign_ins (medium_id, acct_id, created_ts)
SELECT 1 + ((p * 3) % (50 * :scale)),
       p,
       1700000000000000::bigint + p * 800
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p;

INSERT INTO pgbench_fin_loans (id, borrower_id, acct_id, loan_amount, balance, created_ts)
SELECT l,
       1 + ((l * 3) % (:persons_per_scale * :scale)),
       1 + ((l * 3) % (:persons_per_scale * :scale)),
       round((10000.00 + (l % 20) * 1500.00)::numeric, 2),
       round((4000.00 + (l % 20) * 600.00)::numeric, 2),
       1700000000000000::bigint + l * 1500
  FROM generate_series(:client_id + 1, 40 * :scale, :num_workers) AS l;

INSERT INTO pgbench_fin_transfers (id, src_acct, dst_acct, amount, created_ts)
SELECT (p - 1) * 3 + k,
       p,
       CASE
           WHEN k = 1 THEN 1 + (p % (:persons_per_scale * :scale))
           WHEN k = 2 THEN 1 + ((p + 1) % (:persons_per_scale * :scale))
           ELSE pgbench.ldbc_correlated_edge(
               p, (:persons_per_scale * :scale)::bigint, 8::bigint, 0.70::float8, (99 + k)::bigint
           )
       END,
       round((100.00 + ((p * k * 37) % 2500))::numeric, 2),
       1700000000000000::bigint + p * 1000 + k * 250
  FROM generate_series(:client_id + 1, :persons_per_scale * :scale, :num_workers) AS p
  CROSS JOIN generate_series(1, 3) AS k;
