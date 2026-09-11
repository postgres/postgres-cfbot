-- ============================================================
-- RPR EXPLAIN Tests
-- Tests for Row Pattern Recognition EXPLAIN output
-- ============================================================
--
-- Views and tables in this file are intentionally not dropped,
-- so that pg_upgrade/pg_dump can test RPR syntax serialization.
--
-- This test suite validates EXPLAIN output for RPR queries,
-- including NFA statistics shown in EXPLAIN ANALYZE:
--   - NFA States: peak, total, merged
--   - NFA Contexts: peak, total, pruned
--   - NFA: matched (len min/max/avg), mismatched (len min/max/avg)
--   - NFA: absorbed (len min/max/avg), skipped (len min/max/avg)
--   - Pattern deparse formatting
--   - Multiple output formats (text, JSON, XML)
--
-- Test Coverage:
--   Basic NFA Statistics Tests
--   State Statistics Tests
--   Context Statistics Tests
--   Match Length Statistics Tests
--   Mismatch Length Statistics Tests
--   JSON Format Tests
--   XML Format Tests
--   Multiple Partitions Tests
--   Edge Cases
--   Complex Pattern Tests
--   Real-world Pattern Examples
--   Performance-oriented Tests
--   INITIAL vs no INITIAL comparison
--   Quantifier Variations
--   Regression Tests for Statistics Accuracy
--   Alternation Pattern Tests
--   Group Pattern Tests
--   Window Function Combinations
--   DEFINE Expression Variations
--   Large Scale Statistics Verification
--   Nav Mark Lookback/Lookahead (tuplestore trim)
-- ============================================================

-- Filter function to normalize platform-dependent memory values (not NFA statistics).
-- NFA statistics should not change between platforms; if they do, it could
-- indicate issues such as uninitialized memory access.
-- Works for text, JSON, and XML formats.
create function rpr_explain_filter(text) returns setof text
language plpgsql as
$$
declare
    ln text;
begin
    for ln in execute $1
    loop
        -- Normalize platform-dependent memory values
        -- Keep NFA statistics numbers unchanged (they are test assertions)

        -- Text format: "Storage: Memory  Maximum Storage: 18kB"
        if ln ~ 'Storage:.*Maximum Storage:' then
            ln := regexp_replace(ln, '\m\d+kB', 'NkB', 'g');
        end if;

        -- JSON format: "Maximum Storage": 17 (number in kB units)
        if ln ~ '"Maximum Storage":' then
            ln := regexp_replace(ln, '"Maximum Storage": \d+', '"Maximum Storage": 0', 'g');
        end if;

        -- XML format: <Maximum-Storage>17</Maximum-Storage> (number in kB units)
        if ln ~ '<Maximum-Storage>' then
            ln := regexp_replace(ln, '<Maximum-Storage>\d+</Maximum-Storage>', '<Maximum-Storage>0</Maximum-Storage>', 'g');
        end if;

        -- Sort Method memory is platform-dependent (32-bit vs 64-bit)
        if ln ~ 'Sort Method:.*Memory:' then
            ln := regexp_replace(ln, 'Memory: \d+kB', 'Memory: NkB');
        end if;

        return next ln;
    end loop;
end;
$$;

-- Setup: Create test tables
CREATE TABLE rpr_nfa_test (
    id serial,
    v int,
    cat char(1)
);

-- Insert test data: 100 rows with predictable pattern
INSERT INTO rpr_nfa_test (v, cat)
SELECT i,
       CASE
           WHEN i % 5 = 1 THEN 'A'
           WHEN i % 5 = 2 THEN 'B'
           WHEN i % 5 = 3 THEN 'C'
           WHEN i % 5 = 4 THEN 'D'
           ELSE 'E'
       END
FROM generate_series(1, 100) i;

-- Additional test table with more complex patterns
CREATE TABLE rpr_nfa_complex (
    id serial,
    price int,
    trend char(1)  -- U=up, D=down, S=stable
);

INSERT INTO rpr_nfa_complex (price, trend)
VALUES
    (100, 'S'), (105, 'U'), (110, 'U'), (108, 'D'), (112, 'U'),
    (115, 'U'), (113, 'D'), (111, 'D'), (109, 'D'), (110, 'U'),
    (120, 'U'), (125, 'U'), (130, 'U'), (128, 'D'), (126, 'D'),
    (124, 'D'), (122, 'D'), (120, 'D'), (118, 'D'), (119, 'U'),
    (121, 'U'), (123, 'U'), (125, 'U'), (127, 'U'), (129, 'U'),
    (131, 'U'), (133, 'U'), (130, 'D'), (127, 'D'), (124, 'D');

-- ============================================================
-- Basic NFA Statistics Tests
-- ============================================================

-- Simple pattern - should show basic statistics
CREATE VIEW rpr_ev_basic_simple AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS cat = 'A', B AS cat = 'B'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_simple'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS cat = ''A'', B AS cat = ''B''
)');

-- Pattern with no matches - 0 matched
CREATE VIEW rpr_ev_basic_nomatch AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (X Y Z)
    DEFINE X AS cat = 'X', Y AS cat = 'Y', Z AS cat = 'Z'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_nomatch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (X Y Z)
    DEFINE X AS cat = ''X'', Y AS cat = ''Y'', Z AS cat = ''Z''
);');

-- Pattern matching every row - high match count
CREATE VIEW rpr_ev_basic_allrows AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (R)
    DEFINE R AS TRUE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_allrows'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (R)
    DEFINE R AS TRUE
);');

-- Space before parenthesis in pattern deparse
-- Verifies that "A (B | C)" correctly outputs as "a (b | c)" with space
CREATE VIEW rpr_ev_basic_deparse_space AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B | C))
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_deparse_space'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B | C))
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- Sequential alternations at the same depth
-- Verifies that "((B | C) (D | E))" correctly outputs as "(b | c) (d | e)"
CREATE VIEW rpr_ev_basic_deparse_seqalt AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ((B | C) (D | E))*)
    DEFINE A AS v % 5 = 1, B AS v % 5 = 2, C AS v % 5 = 3, D AS v % 5 = 4, E AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_deparse_seqalt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ((B | C) (D | E))*)
    DEFINE A AS v % 5 = 1, B AS v % 5 = 2, C AS v % 5 = 3, D AS v % 5 = 4, E AS v % 5 = 0
);');

-- ALT branch whose tail is a group
-- (A | (B C)+ (D E)+) means A | ((B C)+ (D E)+) by precedence, so the two
-- trailing groups must deparse inside one branch, not as separate branches.
CREATE VIEW rpr_ev_basic_deparse_alttail AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A | (B C)+ (D E)+)
    DEFINE A AS v % 6 = 1, B AS v % 6 = 2, C AS v % 6 = 3, D AS v % 6 = 4, E AS v % 6 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_basic_deparse_alttail'), E'\n')) AS line WHERE line ~ 'PATTERN';
-- The viewdef above deparses the parse tree; this one deparses the compiled
-- element array, which enumerates ALT branches via the SEP terminators.
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A | (B C)+ (D E)+)
    DEFINE A AS v % 6 = 1, B AS v % 6 = 2, C AS v % 6 = 3, D AS v % 6 = 4, E AS v % 6 = 5
);');

-- Quoted identifiers in EXPLAIN pattern deparse
-- Mixed case names must be quoted to preserve round-trip safety
SELECT rpr_explain_filter('
EXPLAIN (COSTS OFF)
SELECT count(*) OVER w
FROM generate_series(1, 10) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ("Start" "Up"+)
    DEFINE "Start" AS TRUE, "Up" AS v > PREV(v)
);');

-- ============================================================
-- State Statistics Tests (peak, total, merged)
-- ============================================================

-- Simple quantifier pattern - A+ with short matches (no merging)
CREATE VIEW rpr_ev_state_simple_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v % 2 = 1
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_simple_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v % 2 = 1
);');

-- Alternation pattern - multiple state branches
CREATE VIEW rpr_ev_state_alt AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C) (D | E))
    DEFINE
        A AS cat = 'A', B AS cat = 'B', C AS cat = 'C',
        D AS cat = 'D', E AS cat = 'E'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C) (D | E))
    DEFINE
        A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C'',
        D AS cat = ''D'', E AS cat = ''E''
);');

-- Complex pattern: quantified sequence with a star element
CREATE VIEW rpr_ev_state_complex AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B* C+)
    DEFINE
        A AS v % 3 = 1,
        B AS v % 3 = 2,
        C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_complex'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B* C+)
    DEFINE
        A AS v % 3 = 1,
        B AS v % 3 = 2,
        C AS v % 3 = 0
);');

-- Grouped pattern with quantifier - state count with grouping
CREATE VIEW rpr_ev_state_group_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_group_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- State explosion pattern - many alternations
CREATE VIEW rpr_ev_state_explosion AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B))
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_explosion'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B))
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Consecutive ALT merge followed by different ALT
-- ((A | B) (A | B) (C | D)) -> (A|B){2} (C|D)
CREATE VIEW rpr_ev_state_alt_merge_alt AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (C | D))
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt_merge_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (C | D))
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Consecutive ALT merge followed by non-ALT element
-- ((A | B) (A | B) C) -> (A|B){2} C
CREATE VIEW rpr_ev_state_alt_merge_nonalt AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt_merge_nonalt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);');

-- ALT prefix/suffix absorbed into GROUP: (A|B) (A|B)+ (A|B) -> (A|B){3,}
CREATE VIEW rpr_ev_state_alt_absorb_group AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B)+ (A | B))
    DEFINE A AS v % 2 = 0, B AS v % 2 = 1
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt_absorb_group'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B)+ (A | B))
    DEFINE A AS v % 2 = 0, B AS v % 2 = 1
);');

-- Folding a copy that sits between two GROUPs leaves those GROUPs
-- adjacent, which nothing had put side by side before, so the GROUP
-- merge runs once more to collect them.  The copy reaches the fold as
-- a suffix of the GROUP before it.
-- (A B)+ A B (A B)+ A B -> (A B){4,}
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ A B (A B)+ A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- A leading copy reaches the fold as a prefix instead
-- A B (A B)+ A B (A B)+ -> (A B){4,}
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B (A B)+ A B (A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- The same with bounded quantifiers
-- (A B){2} A B (A B){2} -> (A B){5}
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B){2} A B (A B){2})
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Three GROUPs and two copies between them
-- (A B)+ A B (A B)+ A B (A B)+ -> (A B){5,}
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ A B (A B)+ A B (A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- High state count - alternation with plus quantifier
CREATE VIEW rpr_ev_state_alt_plus AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C)+ D)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3, D AS v % 4 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt_plus'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C)+ D)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3, D AS v % 4 = 0
);');

-- Early termination: first ALT branch (A) reaches FIN immediately,
-- pruning second branch (A B+) before it can accumulate B repetitions.
CREATE VIEW rpr_ev_state_alt_prune AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | A B)+)
    DEFINE A AS v = 1, B AS v > 1
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_alt_prune'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | A B)+)
    DEFINE A AS v = 1, B AS v > 1
);');

-- Nested quantifiers causing state growth
CREATE VIEW rpr_ev_state_nested_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B)+)+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_nested_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B)+)+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2
);');

-- (A{2,})* must NOT flatten to a*: counts {0} UNION [2, INF) leave 1
-- unreachable.  The planner keeps it as (a{2,})*, not a*.
CREATE VIEW rpr_ev_nested_quant_no_flatten AS
SELECT count(*) OVER w
FROM generate_series(1, 6) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A{2,})*)
    DEFINE A AS v % 3 <> 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_nested_quant_no_flatten'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 6) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A{2,})*)
    DEFINE A AS v % 3 <> 0
);');

-- Overlapping DEFINEs let two branches reach the same state, so this is the
-- only case here with a nonzero merged count; the alt_merge names above mean
-- the optimizer's merge of consecutive ALTs, not this counter.
CREATE VIEW rpr_ev_state_dedup AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_state_dedup'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 3 = 0
);');

-- ============================================================
-- Context Statistics Tests (peak, total, pruned + absorbed/skipped)
-- ============================================================

-- Context absorption with unbounded quantifier at start
CREATE VIEW rpr_ev_ctx_absorb_unbounded AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_unbounded'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Bare unbounded quantifier: A+ absorbs redundant contexts
-- min=1 commits no match until the run ends, so newer contexts absorb in-progress
CREATE VIEW rpr_ev_ctx_absorb_plus AS
SELECT count(*) OVER w
FROM generate_series(1, 10) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v > 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_plus'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 10) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v > 0
);');

-- Bare min=0 quantifier: A* is skipped, not absorbed
-- min=0 commits an empty match at creation, so SKIP (not absorption) removes them
CREATE VIEW rpr_ev_ctx_absorb_star AS
SELECT count(*) OVER w
FROM generate_series(1, 10) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A*)
    DEFINE A AS v > 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_star'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 10) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A*)
    DEFINE A AS v > 0
);');

-- No absorption - bounded quantifier
CREATE VIEW rpr_ev_ctx_no_absorb AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_no_absorb'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Contexts skipped by SKIP PAST LAST ROW
CREATE VIEW rpr_ev_ctx_skip AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v % 10 = 1, B AS v % 10 = 2, C AS v % 10 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_skip'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v % 10 = 1, B AS v % 10 = 2, C AS v % 10 = 3
);');

-- Unbounded group followed by a trailing element
CREATE VIEW rpr_ev_ctx_absorb_group AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_group'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- Fixed-length group absorption: (A B B)+ C
-- B B merged to B{2}; absorbable with fixed-length check
-- step_size=3 (A + B + B); v % 7 cycle gives 2 iterations per match
CREATE VIEW rpr_ev_ctx_absorb_fixedvar AS
SELECT count(*) OVER w
FROM generate_series(1, 70) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B B)+ C)
    DEFINE A AS v % 7 IN (1, 4), B AS v % 7 IN (2, 3, 5, 6), C AS v % 7 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_fixedvar'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 70) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B B)+ C)
    DEFINE A AS v % 7 IN (1, 4), B AS v % 7 IN (2, 3, 5, 6), C AS v % 7 = 0
);');

-- Nested fixed-length group absorption: (A (B C){2} D)+ E
-- step_size = 1 + (1+1)*2 + 1 = 6; v % 13 cycle gives 2 iterations + E
CREATE VIEW rpr_ev_ctx_absorb_nested AS
SELECT count(*) OVER w
FROM generate_series(1, 65) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A (B C){2} D)+ E)
    DEFINE A AS v % 13 IN (1, 7), B AS v % 13 IN (2, 4, 8, 10),
           C AS v % 13 IN (3, 5, 9, 11), D AS v % 13 IN (6, 12),
           E AS v % 13 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_nested'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 65) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A (B C){2} D)+ E)
    DEFINE A AS v % 13 IN (1, 7), B AS v % 13 IN (2, 4, 8, 10),
           C AS v % 13 IN (3, 5, 9, 11), D AS v % 13 IN (6, 12),
           E AS v % 13 = 0
);');

-- Doubly nested fixed-length group absorption: (A ((B C{3}){2} D){2} E)+ F
-- step_size = 1 + ((1+3)*2+1)*2 + 1 = 20; v % 41 cycle gives 2 iterations + F
CREATE VIEW rpr_ev_ctx_absorb_deep AS
SELECT count(*) OVER w
FROM generate_series(1, 82) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A ((B C C C){2} D){2} E)+ F)
    DEFINE A AS v % 41 IN (1, 21),
           B AS v % 41 IN (2, 6, 11, 15, 22, 26, 31, 35),
           C AS v % 41 IN (3,4,5, 7,8,9, 12,13,14, 16,17,18,
                           23,24,25, 27,28,29, 32,33,34, 36,37,38),
           D AS v % 41 IN (10, 19, 30, 39),
           E AS v % 41 IN (20, 40),
           F AS v % 41 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_deep'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 82) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A ((B C C C){2} D){2} E)+ F)
    DEFINE A AS v % 41 IN (1, 21),
           B AS v % 41 IN (2, 6, 11, 15, 22, 26, 31, 35),
           C AS v % 41 IN (3,4,5, 7,8,9, 12,13,14, 16,17,18,
                           23,24,25, 27,28,29, 32,33,34, 36,37,38),
           D AS v % 41 IN (10, 19, 30, 39),
           E AS v % 41 IN (20, 40),
           F AS v % 41 = 0
);');

-- 3-level END chain absorption: ((A (B C){2}){2})+
-- step_size = (1 + (1+1)*2) * 2 = 10; v % 21 cycle gives 2 iterations
-- END chain: END(BC{2}) -> END(A..{2}) -> END(+, ABSORBABLE)
CREATE VIEW rpr_ev_ctx_absorb_endchain AS
SELECT count(*) OVER w
FROM generate_series(1, 42) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A (B C){2}){2})+)
    DEFINE A AS v % 21 IN (1, 6, 11, 16),
           B AS v % 21 IN (2, 4, 7, 9, 12, 14, 17, 19),
           C AS v % 21 IN (3, 5, 8, 10, 13, 15, 18, 20)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_endchain'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 42) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A (B C){2}){2})+)
    DEFINE A AS v % 21 IN (1, 6, 11, 16),
           B AS v % 21 IN (2, 4, 7, 9, 12, 14, 17, 19),
           C AS v % 21 IN (3, 5, 8, 10, 13, 15, 18, 20)
);');

-- No absorption when DEFINE uses FIRST (match_start-dependent)
-- Same pattern as rpr_ev_ctx_absorb_unbounded but with FIRST in DEFINE.
-- Compare: absorbed count should be 0 here vs >0 above.
CREATE VIEW rpr_ev_ctx_no_absorb_first AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0 AND v > FIRST(v)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_no_absorb_first'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0 AND v > FIRST(v)
);');

-- Absorption preserved when DEFINE uses only LAST without offset
-- LAST(v) is match_start-independent (always currentpos), so absorption
-- remains active.  Compare: absorbed count should be >0, like the
-- PREV-only case above.
CREATE VIEW rpr_ev_ctx_absorb_last AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS LAST(v) % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_last'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS LAST(v) % 5 = 0
);');

-- No absorption with compound PREV(FIRST()) (match_start-dependent)
CREATE VIEW rpr_ev_ctx_no_absorb_compound AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0 AND PREV(FIRST(v), 1) IS NOT NULL
);
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0 AND PREV(FIRST(v), 1) IS NOT NULL
);');

-- Variable-length group body absorption: (A+ B)+ C
-- Body is not fixed-length, so the leading A+ is the comparison point
-- (Case 3); the group's own END gets no flag.
CREATE VIEW rpr_ev_ctx_absorb_group_greedy AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+ C)
    DEFINE A AS v % 10 NOT IN (0, 9), B AS v % 10 = 9, C AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_group_greedy'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+ C)
    DEFINE A AS v % 10 NOT IN (0, 9), B AS v % 10 = 9, C AS v % 10 = 0
);');

-- No absorption when the enclosing group is reluctant: (A+ B)+? C
-- Same body, DEFINE and rows as above; only the group quantifier differs.
-- Compare: absorbed count 0 and no marker on a+, vs >0 and a+# above.
CREATE VIEW rpr_ev_ctx_no_absorb_reluctant_group AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+? C)
    DEFINE A AS v % 10 NOT IN (0, 9), B AS v % 10 = 9, C AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_no_absorb_reluctant_group'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+? C)
    DEFINE A AS v % 10 NOT IN (0, 9), B AS v % 10 = 9, C AS v % 10 = 0
);');

-- A fixed count leaves reluctance nothing to decide, so (A+ B){2}? is
-- normalized to (A+ B){2} and absorbs where the unbounded (A+ B)+? above
-- does not.  The two spellings below have to report the same marker and the
-- same absorbed count as each other; that is what the normalization buys.
CREATE VIEW rpr_ev_ctx_absorb_fixed_greedy AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B){2})
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_fixed_greedy'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B){2})
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

CREATE VIEW rpr_ev_ctx_absorb_fixed_reluctant AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B){2}?)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_fixed_reluctant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B){2}?)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Alternation, non-absorbable branch match survives absorption: A+ B | C
-- The dominating A+ run absorbs redundant contexts, but the recorded C matches
-- are not absorbable, so they survive (2 matched, not 0)
CREATE VIEW rpr_ev_ctx_absorb_alt_nonabsorb AS
WITH d(id, flags) AS (
    VALUES (1, ARRAY['A']), (2, ARRAY['A', 'C']), (3, ARRAY['A']),
           (4, ARRAY['A']), (5, ARRAY['A', 'C']), (6, ARRAY['A']))
SELECT id, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM d
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B | C)
    DEFINE A AS 'A' = ANY(flags), B AS 'B' = ANY(flags), C AS 'C' = ANY(flags)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_alt_nonabsorb'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
WITH d(id, flags) AS (
    VALUES (1, ARRAY[''A'']), (2, ARRAY[''A'', ''C'']), (3, ARRAY[''A'']),
           (4, ARRAY[''A'']), (5, ARRAY[''A'', ''C'']), (6, ARRAY[''A'']))
SELECT id, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM d
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B | C)
    DEFINE A AS ''A'' = ANY(flags), B AS ''B'' = ANY(flags), C AS ''C'' = ANY(flags)
);');

-- Alternation, both branches absorbable: A+ C | B+
-- A+ C never completes (C absent) so its A+ run absorbs redundant contexts; the
-- finalized B+ matches on the other branch survive (2 matched, not 0)
CREATE VIEW rpr_ev_ctx_absorb_alt_both AS
WITH d(id, flags) AS (
    VALUES (1, ARRAY['A', 'B']), (2, ARRAY['A', 'B']), (3, ARRAY['A', 'B']),
           (4, ARRAY['A']), (5, ARRAY['A']), (6, ARRAY['A', 'B']),
           (7, ARRAY['A', 'B']), (8, ARRAY['A', 'B']), (9, ARRAY['A']))
SELECT id, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM d
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ C | B+)
    DEFINE A AS 'A' = ANY(flags), B AS 'B' = ANY(flags), C AS 'C' = ANY(flags)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_ctx_absorb_alt_both'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
WITH d(id, flags) AS (
    VALUES (1, ARRAY[''A'', ''B'']), (2, ARRAY[''A'', ''B'']), (3, ARRAY[''A'', ''B'']),
           (4, ARRAY[''A'']), (5, ARRAY[''A'']), (6, ARRAY[''A'', ''B'']),
           (7, ARRAY[''A'', ''B'']), (8, ARRAY[''A'', ''B'']), (9, ARRAY[''A'']))
SELECT id, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM d
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ C | B+)
    DEFINE A AS ''A'' = ANY(flags), B AS ''B'' = ANY(flags), C AS ''C'' = ANY(flags)
);');

-- ============================================================
-- Match Length Statistics Tests
-- ============================================================

-- Fixed length matches - all same length
CREATE VIEW rpr_ev_mlen_fixed AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E)
    DEFINE
        A AS cat = 'A', B AS cat = 'B', C AS cat = 'C',
        D AS cat = 'D', E AS cat = 'E'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_fixed'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E)
    DEFINE
        A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C'',
        D AS cat = ''D'', E AS cat = ''E''
);');

-- Unbounded quantifier over a repeating cycle
CREATE VIEW rpr_ev_mlen_variable AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_variable'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);');

-- Very long matches
CREATE VIEW rpr_ev_mlen_long AS
SELECT count(*) OVER w
FROM generate_series(1, 200) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v <= 195, B AS v > 195
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_long'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 200) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v <= 195, B AS v > 195
);');

-- Uniform match length with mismatches from gap rows (v%20 = 11..15)
CREATE VIEW rpr_ev_mlen_with_mismatch AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS (v % 20 <> 0) AND (v % 20 <= 10 OR v % 20 > 15),
        B AS v % 20 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_with_mismatch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS (v % 20 <> 0) AND (v % 20 <= 10 OR v % 20 > 15),
        B AS v % 20 = 0
);');

-- ============================================================
-- Mismatch Length Statistics Tests
-- ============================================================

-- Pattern with complete match every cycle: 0 mismatched
-- A(1,2,3) B(4,5) C(6) repeats perfectly; X rows are pruned, not mismatched
CREATE VIEW rpr_ev_mlen_no_mismatch AS
SELECT count(*) OVER w
FROM (
    SELECT v,
           CASE WHEN v % 10 IN (1,2,3) THEN 'A'
                WHEN v % 10 IN (4,5) THEN 'B'
                WHEN v % 10 = 6 THEN 'C'
                ELSE 'X' END AS cat
    FROM generate_series(1, 100) AS s(v)
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+ C)
    DEFINE A AS cat = 'A', B AS cat = 'B', C AS cat = 'C'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_no_mismatch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM (
    SELECT v,
           CASE WHEN v % 10 IN (1,2,3) THEN ''A''
                WHEN v % 10 IN (4,5) THEN ''B''
                WHEN v % 10 = 6 THEN ''C''
                ELSE ''X'' END AS cat
    FROM generate_series(1, 100) AS s(v)
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+ C)
    DEFINE A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C''
);');

-- Long partial matches that fail
CREATE VIEW rpr_ev_mlen_long_partial AS
SELECT count(*) OVER w
FROM (
    SELECT i AS v,
           CASE
               WHEN i <= 20 THEN 'A'
               WHEN i <= 25 THEN 'B'
               WHEN i = 26 THEN 'X'  -- breaks the pattern
               WHEN i <= 50 THEN 'A'
               WHEN i <= 55 THEN 'B'
               WHEN i = 56 THEN 'C'  -- completes pattern
               ELSE 'Y'
           END AS cat
    FROM generate_series(1, 60) i
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+ C)
    DEFINE A AS cat = 'A', B AS cat = 'B', C AS cat = 'C'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_mlen_long_partial'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM (
    SELECT i AS v,
           CASE
               WHEN i <= 20 THEN ''A''
               WHEN i <= 25 THEN ''B''
               WHEN i = 26 THEN ''X''  -- breaks the pattern
               WHEN i <= 50 THEN ''A''
               WHEN i <= 55 THEN ''B''
               WHEN i = 56 THEN ''C''  -- completes pattern
               ELSE ''Y''
           END AS cat
    FROM generate_series(1, 60) i
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+ C)
    DEFINE A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C''
);');

-- ============================================================
-- JSON Format Tests
-- ============================================================

-- JSON format output with all statistics
CREATE VIEW rpr_ev_json_basic AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_json_basic'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2
)');

-- JSON format with match length statistics
CREATE VIEW rpr_ev_json_matchlen AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_json_matchlen'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
)');

-- JSON format with mismatch statistics
-- Pattern A B C expects 1,2,3 but gets 1,2,4 twice causing mismatches
CREATE VIEW rpr_ev_json_mismatch AS
SELECT count(*) OVER w
FROM (VALUES (1),(2),(4), (1),(2),(4), (1),(2),(3)) AS t(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v = 1, B AS v = 2, C AS v = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_json_mismatch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON)
SELECT count(*) OVER w
FROM (VALUES (1),(2),(4), (1),(2),(4), (1),(2),(3)) AS t(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v = 1, B AS v = 2, C AS v = 3
)');

-- JSON format with skipped context statistics
-- Alternation pattern with SKIP PAST LAST ROW causes many contexts to be skipped
CREATE VIEW rpr_ev_json_skip AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B))
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_json_skip'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B) (A | B))
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
)');

-- ============================================================
-- XML Format Tests
-- ============================================================

-- XML format output
CREATE VIEW rpr_ev_xml_basic AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_xml_basic'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT XML)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
)');

-- Absorbed contexts and a navigation, neither of which the case above emits,
-- so without this the XML spelling of the absorbed length group and of Nav
-- Mark goes unchecked.
CREATE VIEW rpr_ev_xml_absorb AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+)
    DEFINE A AS v % 10 <> 0 AND PREV(v) IS NOT NULL, B AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_xml_absorb'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT XML)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+)
    DEFINE A AS v % 10 <> 0 AND PREV(v) IS NOT NULL, B AS v % 10 = 0
)');

-- A pattern that mismatches at all, which the two cases above never do, so
-- this is where the mismatch length group appears in XML.
CREATE VIEW rpr_ev_xml_mismatch AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_xml_mismatch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT XML)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 3 = 0
)');

-- ============================================================
-- Multiple Partitions Tests
-- ============================================================

-- Statistics across multiple partitions
CREATE VIEW rpr_ev_part_multi AS
SELECT count(*) OVER w
FROM (
    SELECT p, v
    FROM generate_series(1, 3) p,
         generate_series(1, 30) v
) t
WINDOW w AS (
    PARTITION BY p
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_part_multi'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM (
    SELECT p, v
    FROM generate_series(1, 3) p,
         generate_series(1, 30) v
) t
WINDOW w AS (
    PARTITION BY p
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Different pattern behavior per partition
CREATE VIEW rpr_ev_part_diff AS
SELECT count(*) OVER w
FROM (
    SELECT
        CASE WHEN v <= 25 THEN 1 ELSE 2 END AS p,
        v % 10 AS val
    FROM generate_series(1, 50) v
) t
WINDOW w AS (
    PARTITION BY p
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS val < 5, B AS val >= 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_part_diff'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM (
    SELECT
        CASE WHEN v <= 25 THEN 1 ELSE 2 END AS p,
        v % 10 AS val
    FROM generate_series(1, 50) v
) t
WINDOW w AS (
    PARTITION BY p
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS val < 5, B AS val >= 5
);');

-- ============================================================
-- Edge Cases
-- ============================================================

-- Empty result set
CREATE VIEW rpr_ev_edge_empty AS
SELECT count(*) OVER w
FROM generate_series(1, 0) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v = 1, B AS v = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 0) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v = 1, B AS v = 2
);');

-- Empty matches (length 0): mirror the test_728_* cases in rpr_nfa.sql.
-- Window aggregates over a length-0 frame return 0 / NULL, so the SELECT
-- result alone cannot distinguish "no match" from "empty match"; the
-- "NFA: N matched (len 0/0/0.0)" line in EXPLAIN is the only observable
-- proof that the empty matches were found.

-- (A?){0,3}: min=0, A never matches -> 3 length-0 matches
CREATE VIEW rpr_ev_edge_empty_match_min0 AS
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){0,3})
    DEFINE A AS FALSE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty_match_min0'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){0,3})
    DEFINE A AS FALSE
);');

-- (A?){1,3}: min=1, one empty iteration satisfies min -> 3 length-0 matches
CREATE VIEW rpr_ev_edge_empty_match_min1 AS
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){1,3})
    DEFINE A AS FALSE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty_match_min1'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){1,3})
    DEFINE A AS FALSE
);');

-- (A?){2,3}: min=2 (ISO/IEC 19075-5 7.2.8 STR06 = STRE STRE) -> 3 length-0 matches
CREATE VIEW rpr_ev_edge_empty_match_min2 AS
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE A AS FALSE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty_match_min2'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE A AS FALSE
);');

-- (A?){2,3} mixed: rows 1-2 match A (real), rows 3-4 fall back to empty
CREATE VIEW rpr_ev_edge_empty_match_mixed AS
SELECT count(*) OVER w
FROM generate_series(1, 4) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE A AS v <= 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty_match_mixed'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 4) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE A AS v <= 2
);');

-- (A? B?){2,3}: pure empty multi-element body -> 3 length-0 matches
CREATE VIEW rpr_ev_edge_empty_match_multi AS
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){2,3})
    DEFINE A AS FALSE, B AS FALSE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_empty_match_multi'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 3) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){2,3})
    DEFINE A AS FALSE, B AS FALSE
);');

-- Single row
CREATE VIEW rpr_ev_edge_single_row AS
SELECT count(*) OVER w
FROM generate_series(1, 1) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A)
    DEFINE A AS TRUE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_single_row'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 1) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A)
    DEFINE A AS TRUE
);');

-- Pattern longer than data
CREATE VIEW rpr_ev_edge_pattern_longer AS
SELECT count(*) OVER w
FROM generate_series(1, 5) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E F G H I J)
    DEFINE
        A AS v = 1, B AS v = 2, C AS v = 3, D AS v = 4, E AS v = 5,
        F AS v = 6, G AS v = 7, H AS v = 8, I AS v = 9, J AS v = 10
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_pattern_longer'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 5) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E F G H I J)
    DEFINE
        A AS v = 1, B AS v = 2, C AS v = 3, D AS v = 4, E AS v = 5,
        F AS v = 6, G AS v = 7, H AS v = 8, I AS v = 9, J AS v = 10
);');

-- All rows match as single match
CREATE VIEW rpr_ev_edge_single_match AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS TRUE
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_edge_single_match'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS TRUE
);');

-- ============================================================
-- Complex Pattern Tests
-- ============================================================

-- Nested groups
CREATE VIEW rpr_ev_cpx_nested AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B) C)+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_cpx_nested'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B) C)+)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- Multiple alternations
CREATE VIEW rpr_ev_cpx_multi_alt AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (C | D | E))
    DEFINE
        A AS cat = 'A', B AS cat = 'B', C AS cat = 'C',
        D AS cat = 'D', E AS cat = 'E'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_cpx_multi_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (C | D | E))
    DEFINE
        A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C'',
        D AS cat = ''D'', E AS cat = ''E''
);');

-- Optional elements
CREATE VIEW rpr_ev_cpx_optional AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B? C)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_cpx_optional'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B? C)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3
);');

-- Bounded quantifiers
CREATE VIEW rpr_ev_cpx_bounded AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,5} B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_cpx_bounded'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,5} B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);');

-- Star quantifier
CREATE VIEW rpr_ev_cpx_star AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B* C)
    DEFINE A AS v % 10 = 1, B AS v % 10 IN (2,3,4,5,6,7,8), C AS v % 10 = 9
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_cpx_star'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B* C)
    DEFINE A AS v % 10 = 1, B AS v % 10 IN (2,3,4,5,6,7,8), C AS v % 10 = 9
);');

-- ============================================================
-- Real-world Pattern Examples
-- ============================================================

-- Stock price pattern - V-shape (down then up)
CREATE VIEW rpr_ev_real_vshape AS
SELECT count(*) OVER w
FROM rpr_nfa_complex
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (D+ U+)
    DEFINE D AS trend = 'D', U AS trend = 'U'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_real_vshape'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_complex
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (D+ U+)
    DEFINE D AS trend = ''D'', U AS trend = ''U''
);');

-- Stock price pattern - peak (up, stable, down)
CREATE VIEW rpr_ev_real_peak AS
SELECT count(*) OVER w
FROM rpr_nfa_complex
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (U+ S* D+)
    DEFINE U AS trend = 'U', S AS trend = 'S', D AS trend = 'D'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_real_peak'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_complex
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (U+ S* D+)
    DEFINE U AS trend = ''U'', S AS trend = ''S'', D AS trend = ''D''
);');

-- Consecutive increasing values (using PREV)
CREATE VIEW rpr_ev_real_increasing AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3,})
    DEFINE A AS v > PREV(v) OR PREV(v) IS NULL
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_real_increasing'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3,})
    DEFINE A AS v > PREV(v) OR PREV(v) IS NULL
);');

-- ============================================================
-- Performance-oriented Tests
-- ============================================================

-- Large dataset with simple pattern
CREATE VIEW rpr_ev_perf_large_simple AS
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_perf_large_simple'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Large dataset with absorption
CREATE VIEW rpr_ev_perf_large_absorb AS
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 100 <> 0, B AS v % 100 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_perf_large_absorb'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 1000) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 100 <> 0, B AS v % 100 = 0
);');

-- Alternation with plus quantifier over 500 rows
CREATE VIEW rpr_ev_perf_high_merge AS
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_perf_high_merge'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- ============================================================
-- INITIAL vs no INITIAL comparison
-- ============================================================
-- INITIAL is the only supported mode (SEEK is unimplemented), so omitting the
-- keyword changes nothing at run time and deparse adds it back.  Print the
-- whole viewdef: filtering to the PATTERN line would hide the INITIAL literal,
-- which is the only thing this test can pin.
CREATE VIEW rpr_ev_initial_without AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT pg_get_viewdef('rpr_ev_initial_without'::regclass);

-- ============================================================
-- Quantifier Variations
-- ============================================================

-- Plus quantifier
CREATE VIEW rpr_ev_quant_plus AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v % 4 <> 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_plus'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE A AS v % 4 <> 0
);');

-- Star quantifier (zero or more)
CREATE VIEW rpr_ev_quant_star AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A* B)
    DEFINE A AS v % 4 IN (1, 2), B AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_star'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A* B)
    DEFINE A AS v % 4 IN (1, 2), B AS v % 4 = 3
);');

-- Question mark (zero or one)
CREATE VIEW rpr_ev_quant_question AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A? B C)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_question'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A? B C)
    DEFINE A AS v % 4 = 1, B AS v % 4 = 2, C AS v % 4 = 3
);');

-- Exact count {n}
CREATE VIEW rpr_ev_quant_exact AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_exact'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Range {n,m}
CREATE VIEW rpr_ev_quant_range AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_range'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4} B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- At least {n,}
CREATE VIEW rpr_ev_quant_atleast AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3,} B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_quant_atleast'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3,} B)
    DEFINE A AS v % 10 <> 0, B AS v % 10 = 0
);');

-- ============================================================
-- Regression Tests for Statistics Accuracy
-- ============================================================

-- Verify state count accuracy
-- Pattern A+ B with 20 rows should show predictable state behavior
CREATE VIEW rpr_ev_reg_state_count AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_reg_state_count'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Verify context count with known absorption
CREATE VIEW rpr_ev_reg_ctx_absorb AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B C)
    DEFINE A AS v % 10 IN (1,2,3,4,5,6,7), B AS v % 10 = 8, C AS v % 10 = 9
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_reg_ctx_absorb'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B C)
    DEFINE A AS v % 10 IN (1,2,3,4,5,6,7), B AS v % 10 = 8, C AS v % 10 = 9
);');

-- Verify match length with fixed-length pattern
CREATE VIEW rpr_ev_reg_matchlen AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_reg_matchlen'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- ============================================================
-- Alternation Pattern Tests
-- ============================================================

-- Simple alternation
CREATE VIEW rpr_ev_alt_simple AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) C)
    DEFINE A AS cat = 'A', B AS cat = 'B', C AS cat = 'C'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_simple'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) C)
    DEFINE A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C''
);');

-- Multiple items in alternation
CREATE VIEW rpr_ev_alt_multi_item AS
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C | D) E)
    DEFINE
        A AS cat = 'A', B AS cat = 'B', C AS cat = 'C',
        D AS cat = 'D', E AS cat = 'E'
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_multi_item'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM rpr_nfa_test
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B | C | D) E)
    DEFINE
        A AS cat = ''A'', B AS cat = ''B'', C AS cat = ''C'',
        D AS cat = ''D'', E AS cat = ''E''
);');

-- Alternation with quantifiers
CREATE VIEW rpr_ev_alt_with_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_with_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B)+ C)
    DEFINE A AS v % 3 = 1, B AS v % 3 = 2, C AS v % 3 = 0
);');

-- Multiple alternatives (4+)
CREATE VIEW rpr_ev_alt_four_plus AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A | B | C | D | E)
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, C AS v % 5 = 2, D AS v % 5 = 3, E AS v % 5 = 4
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_four_plus'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A | B | C | D | E)
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, C AS v % 5 = 2, D AS v % 5 = 3, E AS v % 5 = 4
);');

-- Alternation at start
CREATE VIEW rpr_ev_alt_at_start AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B) C D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_at_start'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B) C D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Multiple sequential alternations
CREATE VIEW rpr_ev_alt_sequential AS
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B) C (D | E) F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2, D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_sequential'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 100) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B) C (D | E) F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2, D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);');

-- Quantified alternatives
CREATE VIEW rpr_ev_alt_quantified AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A+ | B+) C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_quantified'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A+ | B+) C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);');

-- Alternation at end
CREATE VIEW rpr_ev_alt_at_end AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (C | D))
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_at_end'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A B (C | D))
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Nested ALT at start of branch inside outer ALT
-- Pattern: (A ((B | C) D | E)) - preceding VAR + inner ALT as first branch element
CREATE VIEW rpr_ev_alt_nested_start AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ((B | C) D | E))
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, C AS v % 5 = 2, D AS v % 5 = 3, E AS v % 5 = 4
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_nested_start'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A ((B | C) D | E))
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, C AS v % 5 = 2, D AS v % 5 = 3, E AS v % 5 = 4
);');

-- Nested ALT at end of branch inside outer ALT
-- Pattern: (C (A | B) | D) - inner ALT is last element in outer branch
CREATE VIEW rpr_ev_alt_nested_end AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C (A | B) | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_nested_end'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C (A | B) | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Quantified group as the first alternation branch
-- Pattern: ((A B)+ | C) - leading group branch must open the enclosing paren
CREATE VIEW rpr_ev_alt_grp_first AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_first'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Quantified group as the last alternation branch
-- Pattern: (C | (A B)+) - trailing group branch, no separator follows
CREATE VIEW rpr_ev_alt_grp_last AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_last'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Quantified group as the middle branch of a three-way alternation
-- Pattern: (C | (A B)+ | D) - separator before D must survive the group branch
CREATE VIEW rpr_ev_alt_grp_mid AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+ | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_mid'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+ | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Quantified group as the first branch of a three-way alternation
-- Pattern: ((A B)+ | C | D) - leading group branch with two following branches
CREATE VIEW rpr_ev_alt_grp_first3 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | C | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_first3'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | C | D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Bounded-quantifier group as the first alternation branch
-- Pattern: ((A B){2} | C) - leading group branch with a range quantifier
CREATE VIEW rpr_ev_alt_grp_bounded AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){2} | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_bounded'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B){2} | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Two quantified groups in one alternation
-- Pattern: ((A B)+ | (C D)+) - both branches are groups
CREATE VIEW rpr_ev_alt_grp_both AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | (C D)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_both'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B)+ | (C D)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Leading group branch in an alternation nested in a sequence
-- Pattern: (((A B)+ | C) D) - inner alternation opens with a group branch
CREATE VIEW rpr_ev_alt_grp_seq_head AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B)+ | C) D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_seq_head'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B)+ | C) D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Trailing group branch in an alternation nested in a sequence
-- Pattern: ((C | (A B)+) D) - group as last branch, then a sequence element
CREATE VIEW rpr_ev_alt_grp_seq_tail AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((C | (A B)+) D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_seq_tail'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((C | (A B)+) D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Quantified alternation whose first branch is a quantified group
-- Pattern: (((A B){2} | C)+) - single-ALT group wraps a leading group branch
CREATE VIEW rpr_ev_alt_grp_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B){2} | C)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B){2} | C)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Unit (1,1) group as an alternation branch (emits no BEGIN/END)
-- Pattern: ((A B) | C) - control: a {1,1} group emits no BEGIN/END, so the
-- branch deparses as a plain sequence
CREATE VIEW rpr_ev_alt_grp_unit AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B) | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_unit'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A B) | C)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Quantified variable as the first alternation branch
-- Pattern: (A+ | C) - control: a quantified variable as the leading branch;
-- the ALT supplies the enclosing parentheses
CREATE VIEW rpr_ev_alt_var_first AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ | C)
    DEFINE A AS v % 4 = 0, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_var_first'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+ | C)
    DEFINE A AS v % 4 = 0, C AS v % 4 = 2
);');

-- Quantified group as the last branch of a three-way alternation
-- Pattern: (C | D | (A B)+) - control: trailing group needs no separator
CREATE VIEW rpr_ev_alt_grp_last3 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | D | (A B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_last3'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | D | (A B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Alternation nested in a leading branch must not swallow the trailing branch
-- Pattern: (D (A | B) | E) - inherited limit bounds the inner alternation
CREATE VIEW rpr_ev_alt_inner_bounded AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (D (A | B) | E)
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, D AS v % 5 = 2, E AS v % 5 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_inner_bounded'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (D (A | B) | E)
    DEFINE A AS v % 5 = 0, B AS v % 5 = 1, D AS v % 5 = 2, E AS v % 5 = 3
);');

-- Group mid-branch followed by a sequence element needs no separator before it
-- Pattern: (C | (A B)+ D) - only the branch end takes a SEP, not D
CREATE VIEW rpr_ev_alt_grp_then_seq AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+ D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_grp_then_seq'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (C | (A B)+ D)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2, D AS v % 4 = 3
);');

-- Quantified group wrapping a lone alternation: the ALT supplies the parens
-- Pattern: ((A | B)+) - loneAlt path, single pair of parens
CREATE VIEW rpr_ev_grp_lone_alt AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_lone_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1
);');

-- Quantified group wrapping a sequence whose last element is an alternation
-- Pattern: ((A (B | C))+) - group paren plus a nested alternation paren
CREATE VIEW rpr_ev_grp_seq_alt AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B | C))+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_seq_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B | C))+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Quantified group wrapping a sequence whose first element is an alternation
-- Pattern: (((A | B) C)+) - leading nested alternation inside a group sequence
CREATE VIEW rpr_ev_grp_alt_seq AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B) C)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_alt_seq'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B) C)+)
    DEFINE A AS v % 4 = 0, B AS v % 4 = 1, C AS v % 4 = 2
);');

-- Alternation non-last in a non-last branch, stacked three deep: each level's
-- inherited limit must bound the inner alternation against the next branch
-- Pattern: (((A | B) C | D) E | F) - three nested inherited-limit boundaries
CREATE VIEW rpr_ev_alt_stack3 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B) C | D) E | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_stack3'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B) C | D) E | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);');

-- Same interaction stacked four deep, to exercise the induction one step further
-- Pattern: ((((A | B) C | D) E | F) G | H) - four nested inherited-limit boundaries
CREATE VIEW rpr_ev_alt_stack4 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((((A | B) C | D) E | F) G | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_stack4'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((((A | B) C | D) E | F) G | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);');

-- Three-deep stack whose innermost branch is a quantified group: the group's
-- skip-target jump must not be mistaken for a branch separator at any depth
-- Pattern: (((A | B)+ C | D) E | F) - inherited limit plus loneAlt at the base
CREATE VIEW rpr_ev_alt_stack3_grp AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B)+ C | D) E | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_stack3_grp'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A | B)+ C | D) E | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);');

-- Alternation trailing a paren-less sequence (last element of a non-last
-- branch, no same-depth sibling to bound it), nested three deep
-- Pattern: (A (B (C | D) | E) | F) - each inner alternation is branch-tail
CREATE VIEW rpr_ev_alt_tail3 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C | D) | E) | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_tail3'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C | D) | E) | F)
    DEFINE A AS v % 6 = 0, B AS v % 6 = 1, C AS v % 6 = 2,
           D AS v % 6 = 3, E AS v % 6 = 4, F AS v % 6 = 5
);');

-- Same branch-tail alternation nested four deep
-- Pattern: (A (B (C (D | E) | F) | G) | H) - branch-tail alternation x4
CREATE VIEW rpr_ev_alt_tail4 AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C (D | E) | F) | G) | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_tail4'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C (D | E) | F) | G) | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);');

-- A nested alternation tail neighbouring a multi-element sequence branch: the
-- branch boundary must split "...branch-tail ALT" from a plain "G A" sequence
-- Pattern: (A (B (C (D | E) | F) | G A) | H) - seq branch beside an ALT tail
CREATE VIEW rpr_ev_alt_tail_seqbranch AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C (D | E) | F) | G A) | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_tail_seqbranch'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A (B (C (D | E) | F) | G A) | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);');

-- A nested alternation that is sibling-bounded by a trailing sequence element
-- at the outer level (the ALT is not the branch tail; G follows it in-branch)
-- Pattern: ((A (B (C | D) | E) | F) G | H) - ALT bounded by a following element
CREATE VIEW rpr_ev_alt_mid_seqtail AS
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B (C | D) | E) | F) G | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_alt_mid_seqtail'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 20) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B (C | D) | E) | F) G | H)
    DEFINE A AS v % 8 = 0, B AS v % 8 = 1, C AS v % 8 = 2, D AS v % 8 = 3,
           E AS v % 8 = 4, F AS v % 8 = 5, G AS v % 8 = 6, H AS v % 8 = 7
);');

-- ============================================================
-- Group Pattern Tests
-- ============================================================

-- Simple group
CREATE VIEW rpr_ev_grp_simple AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_simple'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Group with bounded quantifier
CREATE VIEW rpr_ev_grp_bounded AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_bounded'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B){2,4})
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Nested groups
CREATE VIEW rpr_ev_grp_nested AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B){2})+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_nested'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B){2})+)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Deep nesting (3+ levels)
CREATE VIEW rpr_ev_grp_deep AS
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((((A | B)+)+)+)
    DEFINE A AS v % 2 = 0, B AS v % 2 = 1
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_deep'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 40) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((((A | B)+)+)+)
    DEFINE A AS v % 2 = 0, B AS v % 2 = 1
);');

-- Bounded quantifier on alternation
CREATE VIEW rpr_ev_grp_bounded_alt AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B){2,3} C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_bounded_alt'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A | B){2,3} C)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);');

-- Nested groups with quantifiers
CREATE VIEW rpr_ev_grp_nested_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B)+ C)*)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_nested_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A B)+ C)*)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);');

-- Partial nested quantification
CREATE VIEW rpr_ev_grp_partial_quant AS
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B C)+)*)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_grp_partial_quant'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 60) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN ((A (B C)+)*)
    DEFINE A AS v % 3 = 0, B AS v % 3 = 1, C AS v % 3 = 2
);');

-- ============================================================
-- Window Function Combinations
-- ============================================================

-- count(*) with pattern
CREATE VIEW rpr_ev_wfn_count AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_wfn_count'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- first_value with pattern
CREATE VIEW rpr_ev_wfn_first_value AS
SELECT first_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_wfn_first_value'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT first_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- last_value with pattern
CREATE VIEW rpr_ev_wfn_last_value AS
SELECT last_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_wfn_last_value'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT last_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- Multiple window functions
CREATE VIEW rpr_ev_wfn_multi AS
SELECT
    count(*) OVER w,
    first_value(v) OVER w,
    last_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_wfn_multi'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT
    count(*) OVER w,
    first_value(v) OVER w,
    last_value(v) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v % 5 <> 0, B AS v % 5 = 0
);');

-- ============================================================
-- DEFINE Expression Variations
-- ============================================================

-- Complex boolean expressions
CREATE VIEW rpr_ev_def_complex_bool AS
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS (v % 5 <> 0) AND (v % 3 <> 0),
        B AS (v % 5 = 0) OR (v % 3 = 0)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_def_complex_bool'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 50) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS (v % 5 <> 0) AND (v % 3 <> 0),
        B AS (v % 5 = 0) OR (v % 3 = 0)
);');

-- Using PREV function
CREATE VIEW rpr_ev_def_prev AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (S U+ D+)
    DEFINE
        S AS TRUE,
        U AS v > PREV(v),
        D AS v < PREV(v)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_def_prev'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (S U+ D+)
    DEFINE
        S AS TRUE,
        U AS v > PREV(v),
        D AS v < PREV(v)
);');

-- Using 1-arg PREV (implicit offset 1)
CREATE VIEW rpr_ev_nav_prev1 AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS v > PREV(v)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_nav_prev1'), E'\n')) AS line WHERE line ~ 'PATTERN|DEFINE|PREV|NEXT';

-- Using 1-arg NEXT (implicit offset 1)
CREATE VIEW rpr_ev_nav_next1 AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS v < NEXT(v)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_nav_next1'), E'\n')) AS line WHERE line ~ 'PATTERN|DEFINE|PREV|NEXT';

-- Using 2-arg PREV (explicit offset)
CREATE VIEW rpr_ev_nav_prev2 AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS v > PREV(v, 2)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_nav_prev2'), E'\n')) AS line WHERE line ~ 'PATTERN|DEFINE|PREV|NEXT';

-- Using 2-arg NEXT (explicit offset)
CREATE VIEW rpr_ev_nav_next2 AS
SELECT count(*) OVER w
FROM generate_series(1, 30) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B+)
    DEFINE
        A AS TRUE,
        B AS v < NEXT(v, 2)
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_nav_next2'), E'\n')) AS line WHERE line ~ 'PATTERN|DEFINE|PREV|NEXT';

-- Using NULL comparisons
CREATE VIEW rpr_ev_def_null AS
SELECT count(*) OVER w
FROM (
    SELECT CASE WHEN v % 5 = 0 THEN NULL ELSE v END AS v
    FROM generate_series(1, 30) v
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v IS NOT NULL, B AS v IS NULL
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_def_null'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM (
    SELECT CASE WHEN v % 5 = 0 THEN NULL ELSE v END AS v
    FROM generate_series(1, 30) v
) t
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE A AS v IS NOT NULL, B AS v IS NULL
);');

-- ============================================================
-- Large Scale Statistics Verification
-- ============================================================

-- 500 rows - verify statistics scale correctly
CREATE VIEW rpr_ev_scale_500rows AS
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B C)
    DEFINE A AS v % 10 < 7, B AS v % 10 = 7, C AS v % 10 = 8
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_scale_500rows'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B C)
    DEFINE A AS v % 10 < 7, B AS v % 10 = 7, C AS v % 10 = 8
);');

-- High match count scenario
CREATE VIEW rpr_ev_scale_high_match AS
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_scale_high_match'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE A AS v % 2 = 1, B AS v % 2 = 0
);');

-- Sparse five-element pattern over 500 rows
CREATE VIEW rpr_ev_scale_high_skip AS
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E)
    DEFINE
        A AS v % 100 = 1,
        B AS v % 100 = 2,
        C AS v % 100 = 3,
        D AS v % 100 = 4,
        E AS v % 100 = 5
);
SELECT line FROM unnest(string_to_array(pg_get_viewdef('rpr_ev_scale_high_skip'), E'\n')) AS line WHERE line ~ 'PATTERN';
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
SELECT count(*) OVER w
FROM generate_series(1, 500) AS s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E)
    DEFINE
        A AS v % 100 = 1,
        B AS v % 100 = 2,
        C AS v % 100 = 3,
        D AS v % 100 = 4,
        E AS v % 100 = 5
);');

-- ============================================================
-- Nav Mark Lookback/Lookahead Tests
-- Verifies planner-computed navigation offsets for tuplestore trim.
-- Lookback: how far back from currentpos (PREV, LAST, compound PREV_LAST/NEXT_LAST).
-- Lookahead: how far forward from match_start (FIRST, compound PREV_FIRST/NEXT_FIRST).
-- ============================================================

-- Prepare statement for host variable offset test below
PREPARE rpr_nav_offset_prep(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > PREV(v, $1)
);

-- No navigation function
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > 0
);

-- NEXT only: no backward navigation
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v < NEXT(v)
);

-- PREV(v): implicit offset 1
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > PREV(v)
);

-- PREV(v, 3): explicit constant offset 3
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > PREV(v, 3)
);

-- PREV(v, 1 + 1): a foldable offset has to arrive as a constant, or the trim
-- bound would print "runtime" instead of 2
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > PREV(v, 1 + 1)
);

-- Two PREV with different offsets: max(1, 5) = 5
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v, 1) < v AND PREV(v, 5) < v
);

-- Host variable offset: custom plan resolves $1=2 to constant 2
EXPLAIN (COSTS OFF) EXECUTE rpr_nav_offset_prep(2);

-- Force generic plan: offset becomes "runtime" (Param node)
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE rpr_nav_offset_prep(2);
RESET plan_cache_mode;
DEALLOCATE rpr_nav_offset_prep;

-- EXPLAIN (GENERIC_PLAN) of an unbound parameter offset must not evaluate the
-- parameter: the offset stays "runtime" instead of failing with "no value
-- found for parameter 1".
EXPLAIN (GENERIC_PLAN, COSTS OFF)
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > PREV(v, $1)
);

-- FIRST(v) with no offset (references the match start row)
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > FIRST(v)
);

-- FIRST(v, 5): forward reach 5
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > FIRST(v, 5)
);

-- The same forward reach in the XML and JSON forms.  Only the text form is
-- exercised elsewhere, so the tag itself has no coverage.
EXPLAIN (COSTS OFF, FORMAT XML) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > FIRST(v, 5)
);

EXPLAIN (COSTS OFF, FORMAT JSON) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS v > FIRST(v, 5)
);

-- LAST(v, 1): backward reach 1, same as PREV(v, 1)
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS LAST(v, 1) > 0
);

-- LAST(v) without offset + PREV(v): no match_start dependency, offset 1
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS LAST(v) > PREV(v)
);

-- Compound PREV(FIRST(val, 1), 2): lookback from match_start, firstOffset = 1-2 = -1
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, 1), 2) > 0
);

-- Compound NEXT(FIRST(val), 3): firstOffset = 0+3 = 3
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v), 3) > 0
);

-- Compound PREV(LAST(val), 2): lookback = 0+2 = 2
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(LAST(v), 2) > 0
);

-- Compound NEXT(LAST(val, 1), 3): lookback = max(1-3, 0) = 0
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(v, 1), 3) > 0
);

-- Compound forms with the outer offset left out, which defaults to 1: each of
-- the four arms combines it with the inner offset differently, and the value
-- decides both the trim bound and the row the navigation lands on.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, 2)) > 0
);

EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v, 2)) > 0
);

EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(LAST(v, 2)) > 0
);

EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(v, 2)) > 0
);

-- Compound PREV(LAST(val, N), M): constant near-overflow (N+M just fits int64)
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(LAST(v, 4611686018427387903), 4611686018427387903) IS NOT NULL
);

-- Compound PREV(LAST(val, N), M): constant overflow -> retain all
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(LAST(v, 4611686018427387904), 4611686018427387904) IS NOT NULL
);

-- Compound NEXT(FIRST(val, N), M): constant lookahead overflow -> infinite
-- N + M overflows int64; forward reach is unbounded, displayed as infinite.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v, 4611686018427387904), 4611686018427387904) IS NOT NULL
);

-- A navigation with a negative offset cannot run, so it contributes no reach
-- and its dimension reports nothing at all.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, -3), 2) IS NOT NULL
);

-- The same query errors once it runs, since execution validates the offset.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, -3), 2) IS NOT NULL
);

-- Same at the int64 limit, where the reach subtraction would otherwise wrap.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, (-9223372036854775807)::int8), 2) IS NOT NULL
);

-- The other dimension keeps its own aggregate.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v, 5) IS NOT NULL AND FIRST(v, -1) IS NOT NULL
);

-- A null offset cannot run either, so it too contributes no reach.  Resolving
-- it to a placeholder 0 would instead let it join the aggregate and displace
-- the offset of the navigation that can run.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(v, 7) IS NOT NULL AND PREV(FIRST(v, NULL::int), 2) IS NOT NULL
);

-- And it errors once it runs, for the same reason the negative one does.
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(v, 7) IS NOT NULL AND PREV(FIRST(v, NULL::int), 2) IS NOT NULL
);

-- Dropping a navigation must not disturb the kind the survivor reports.  An
-- overflowing lookback still retains every row.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(v, -1) IS NOT NULL
            AND PREV(LAST(v, 4611686018427387904), 4611686018427387904) IS NOT NULL
);

-- And a parameter offset beside a dropped one is still settled per scan.
PREPARE test_dropped_with_runtime(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(v, -1) IS NOT NULL AND PREV(v, $1) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE test_dropped_with_runtime(2);
RESET plan_cache_mode;
DEALLOCATE test_dropped_with_runtime;

-- NEXT(LAST()) reaches the same subtraction on the lookback side.
EXPLAIN (COSTS OFF) SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(v, 2), (-9223372036854775807)::int8) IS NOT NULL
);

-- Compound PREV(LAST(val, $1), $2): parameter lookback overflow -> retain all
-- EXPLAIN shows "runtime" (plan-level); EXPLAIN ANALYZE shows "retain all"
-- (executor-resolved).
PREPARE test_overflow_lookback(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(LAST(v, $1), $2) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE test_overflow_lookback(4611686018427387904, 4611686018427387904);
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
    EXECUTE test_overflow_lookback(4611686018427387904, 4611686018427387904)');
RESET plan_cache_mode;
DEALLOCATE test_overflow_lookback;

-- Compound NEXT(FIRST(val, $1), $2): parameter lookahead overflow -> infinite
PREPARE test_overflow_lookahead(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v, $1), $2) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
    EXECUTE test_overflow_lookahead(4611686018427387904, 4611686018427387904)');
RESET plan_cache_mode;
DEALLOCATE test_overflow_lookahead;

-- Runtime (non-overflow) lookahead: a small parameter offset under a generic
-- plan keeps the FIRST offset unresolved at plan time, so EXPLAIN reports
-- "Nav Mark Lookahead: runtime" rather than a concrete value.
PREPARE p_first_runtime(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v, $1), $2) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXPLAIN (COSTS OFF) EXECUTE p_first_runtime(1, 1);
RESET plan_cache_mode;
DEALLOCATE p_first_runtime;

-- PREV(v) + PREV(v, $1): the implicit lookback of 1 has to count even when the
-- explicit offset resolves to 0, or PREV(v) would fail with "cannot fetch row
-- before mark".  A generic plan settles the reach per scan instead of at init.
SET plan_cache_mode = force_generic_plan;
PREPARE test_prev_implicit_offset(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v) IS NOT NULL AND PREV(v, $1) IS NOT NULL
);
EXECUTE test_prev_implicit_offset(0);
DEALLOCATE test_prev_implicit_offset;
RESET plan_cache_mode;

-- NEEDS_EVAL executor offset paths: a Param nav offset stays non-Const under a
-- generic plan, so build_define_offsets() marks the offset NEEDS_EVAL and
-- resolve_nav_offsets() settles it once per scan.  Each query below exercises
-- a different navigation arm of that walker.

-- Simple FIRST(v, $1): forward-reach FIRST arm.
PREPARE test_eval_first(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS FIRST(v, $1) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXECUTE test_eval_first(1);
RESET plan_cache_mode;
DEALLOCATE test_eval_first;

-- Bare LAST(v, $1): backward-reach LAST-with-offset arm.
PREPARE test_eval_last(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS LAST(v, $1) >= 0
);
SET plan_cache_mode = force_generic_plan;
EXECUTE test_eval_last(1);
RESET plan_cache_mode;
DEALLOCATE test_eval_last;

-- Compound NEXT(LAST(v, $1), $2): backward-reach NEXT_LAST arm.
PREPARE test_eval_nextlast(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(LAST(v, $1), $2) > 0
);
SET plan_cache_mode = force_generic_plan;
EXECUTE test_eval_nextlast(1, 1);
RESET plan_cache_mode;
DEALLOCATE test_eval_nextlast;

-- Compound PREV(FIRST(v, $1), $2): forward-reach PREV_FIRST arm.
PREPARE test_eval_prevfirst(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(FIRST(v, $1), $2) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXECUTE test_eval_prevfirst(1, 1);
-- Observe the arm rather than only run it.  At plan time the line reads
-- "runtime"; once resolved it reads inner - outer, so 2 here is the PREV_FIRST
-- subtraction.  A bare FIRST would report the inner offset alone.
EXPLAIN (COSTS OFF) EXECUTE test_eval_prevfirst(3, 1);
SELECT rpr_explain_filter('
EXPLAIN (ANALYZE, BUFFERS OFF, COSTS OFF, TIMING OFF, SUMMARY OFF)
EXECUTE test_eval_prevfirst(3, 1);');
RESET plan_cache_mode;
DEALLOCATE test_eval_prevfirst;

-- Runtime error: negative offset at execution time
PREPARE test_runtime_neg_offset(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v, $1) IS NOT NULL
);
EXECUTE test_runtime_neg_offset(-1);
DEALLOCATE test_runtime_neg_offset;

-- The same at generic-plan resolution time, and for each half of a compound
-- navigation on its own.
PREPARE test_runtime_neg_compound_offset(int8, int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS NEXT(FIRST(v, $1), $2) IS NOT NULL
);
SET plan_cache_mode = force_generic_plan;
EXECUTE test_runtime_neg_compound_offset(1, -1);
EXECUTE test_runtime_neg_compound_offset(-1, 1);
RESET plan_cache_mode;
DEALLOCATE test_runtime_neg_compound_offset;

-- Runtime error: null offset at execution time
PREPARE test_runtime_null_offset(int8) AS
SELECT count(*) OVER w
FROM generate_series(1,10) s(v)
WINDOW w AS (
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A+)
    DEFINE A AS PREV(v, $1) IS NOT NULL
);
EXECUTE test_runtime_null_offset(NULL);
DEALLOCATE test_runtime_null_offset;

-- A correlated PARAM_EXEC nav offset (reaching the offset via SRF inlining) is
-- resolved per scan by resolve_nav_offsets(); after execution EXPLAIN ANALYZE
-- must display the concrete resolved bound (a number), not "runtime" -- that is,
-- navMaxOffsetKind resolves to FIXED.  Plain EXPLAIN of the same query shows
-- "runtime"; only ANALYZE exercises the per-scan clear.
CREATE TABLE rpr_exp_srf (v int);
INSERT INTO rpr_exp_srf SELECT generate_series(1, 10);
CREATE FUNCTION rpr_exp_srf_f(k int) RETURNS SETOF bigint AS $$
  SELECT count(*) OVER w
  FROM rpr_exp_srf
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (A+) DEFINE A AS v > PREV(v, k))
$$ LANGUAGE sql STABLE;
SELECT t FROM rpr_explain_filter(
  'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
   SELECT g.n, max(s) FROM (VALUES (2), (2)) g(n), LATERAL rpr_exp_srf_f(g.n) s
   GROUP BY g.n') AS t
WHERE t LIKE '%Nav Mark Lookback%';
DROP FUNCTION rpr_exp_srf_f(int);
-- The kind a scan settles on is per scan, not sticky.  An outer offset that
-- overflows int64 gives up on the trim for that scan alone, and EXPLAIN
-- ANALYZE reports what the last rescan left behind: the same three offsets
-- in a different order have to read differently, retain all when the
-- overflow runs last and a bound again when a smaller offset follows it.
CREATE FUNCTION rpr_exp_srf_cmp(k int8) RETURNS SETOF bigint AS $$
  SELECT count(*) OVER w
  FROM rpr_exp_srf
  WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
               PATTERN (A B+) DEFINE B AS v > PREV(LAST(v, 1), k))
$$ LANGUAGE sql STABLE;
SELECT t FROM rpr_explain_filter(
  'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
   SELECT g.n, max(s)
   FROM (VALUES (1::int8), (3::int8), (9223372036854775807::int8)) g(n),
        LATERAL rpr_exp_srf_cmp(g.n) s
   GROUP BY g.n') AS t
WHERE t LIKE '%Nav Mark Lookback%';
SELECT t FROM rpr_explain_filter(
  'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF)
   SELECT g.n, max(s)
   FROM (VALUES (1::int8), (9223372036854775807::int8), (3::int8)) g(n),
        LATERAL rpr_exp_srf_cmp(g.n) s
   GROUP BY g.n') AS t
WHERE t LIKE '%Nav Mark Lookback%';
DROP FUNCTION rpr_exp_srf_cmp(int8);
DROP TABLE rpr_exp_srf;
