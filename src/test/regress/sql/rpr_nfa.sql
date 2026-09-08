-- ============================================================
-- RPR NFA Tests
-- Tests for Row Pattern Recognition NFA Runtime Execution
-- ============================================================
--
-- This test suite validates the NFA (Non-deterministic Finite
-- Automaton) runtime execution engine in execRPR.c, driven by
-- update_reduced_frame() in nodeWindowAgg.c.
--
-- Test Strategy:
--   Diagonal pattern style using ARRAY flags to explicitly
--   control which pattern variables match at each row.
--
-- Test Coverage:
--   Basic NFA Flow (match->absorb->advance)
--   Absorption Optimization
--   Context Lifecycle Management
--   Advance Phase (Epsilon Transitions)
--   Match Phase (Variable Matching)
--   Frame Boundary Handling
--   State Management (Deduplication)
--   Statistics and Diagnostics
--   Quantifier Runtime Behavior
--   Pathological Pattern Protection
--   Alternation Runtime Behavior
--   Deep Nested Groups
--   SKIP Options (Runtime)
--   INITIAL Mode (Runtime)
--   Frame Boundary Variations
--   Special Partition Cases
--   DEFINE Special Cases
--   Absorption Dynamic Flags
--   Zero-Consumption Cycle Detection
--   Standard Clause 7: Formal Pattern Matching Rules
--
-- Responsibility:
--   - NFA runtime execution paths
--   - Context/State lifecycle management
--   - Runtime boundary conditions and protections
--
-- NOT tested here (covered in other files):
--   - Pattern parsing/optimization (rpr_base.sql)
--   - EXPLAIN output (rpr_explain.sql)
--   - PREV/NEXT semantics (rpr.sql)
-- ============================================================

-- ============================================================
-- Basic NFA Flow
-- ============================================================

-- Simple sequential pattern
WITH test_sequential AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['_'])  -- No match
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_sequential
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Quantified pattern (A+ B+ C+)
WITH test_quantified AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['B']),
        (6, ARRAY['C']),
        (7, ARRAY['C']),
        (8, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quantified
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B+ C+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Optional pattern (A B? C)
WITH test_optional AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['C']),  -- B skipped
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['C']),  -- B matched
        (6, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_optional
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B? C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Alternation pattern (A (B|C) D)
WITH test_alternation AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),  -- First branch
        (3, ARRAY['D']),
        (4, ARRAY['A']),
        (5, ARRAY['C']),  -- Second branch
        (6, ARRAY['D']),
        (7, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alternation
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A (B | C) D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ============================================================
-- Absorption Optimization
-- ============================================================

-- Absorbable pattern (A+)
WITH test_absorbable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_absorbable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Mixed absorbable/non-absorbable ((A+) | B)
WITH test_mixed_absorption AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_mixed_absorption
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+) | B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- State coverage (same elemIdx, different count)
WITH test_state_coverage AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_state_coverage
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{2,} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant pattern (A+?) - not absorbable
-- Compare with greedy A+ above: reluctant excluded from absorption.
-- Each context produces minimum match independently.
WITH test_reluctant_absorption AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_absorption
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+?)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Absorption with fixed suffix: A+ B
WITH test_absorb_suffix AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_suffix
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Per-branch absorption with ALT: B+ C | B+ D
WITH test_absorb_alt AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B']),
        (4, ARRAY['D']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_alt
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (B+ C | B+ D)
    DEFINE
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Non-absorbable: A B+ (unbounded not in first position)
WITH test_no_absorb AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['B']),
        (4, ARRAY['B']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_no_absorb
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- GROUP merge enables absorption: (A B) (A B)+ optimized to (A B){2,}
WITH test_absorb_group AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_group
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B) (A B)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Two consecutive unbounded groups: (A B)+ (C D)+
-- The leading group (A B)+ is absorbable (unbounded multi-element); (C D)+ is
-- a distinct sibling group that does not merge with it.  When the leading group
-- exits into the sibling, its body leaf-VAR count must be cleared so it does
-- not leak into the sibling's shared depth slot.
WITH test_absorb_two_groups AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['C']),
        (8, ARRAY['D'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_two_groups
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ (C D)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Fixed-length group absorption: (A B{2})+ C
-- B{2} has min == max, equivalent to unrolling to (A B B)+ C
WITH test_absorb_fixedlen AS (
    SELECT * FROM (VALUES
        (1,  ARRAY['A']),
        (2,  ARRAY['B']),
        (3,  ARRAY['B']),
        (4,  ARRAY['A']),
        (5,  ARRAY['B']),
        (6,  ARRAY['B']),
        (7,  ARRAY['A']),
        (8,  ARRAY['B']),
        (9,  ARRAY['B']),
        (10, ARRAY['C']),
        (11, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_fixedlen
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B{2})+ C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Consecutive vars merged to fixed-length: (A B B)+ -> (A B{2})+
WITH test_absorb_consecutive AS (
    SELECT * FROM (VALUES
        (1,  ARRAY['A']),
        (2,  ARRAY['B']),
        (3,  ARRAY['B']),
        (4,  ARRAY['A']),
        (5,  ARRAY['B']),
        (6,  ARRAY['B']),
        (7,  ARRAY['A']),
        (8,  ARRAY['B']),
        (9,  ARRAY['B']),
        (10, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_consecutive
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B B)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested fixed-length group absorption: (A (B C){2} D)+ E
-- Inner group {2} has min == max; absorbable via recursive check
-- step_size = 1 + (1+1)*2 + 1 = 6
WITH test_absorb_nested_fixedlen AS (
    SELECT * FROM (VALUES
        (1,  ARRAY['A']),
        (2,  ARRAY['B']),
        (3,  ARRAY['C']),
        (4,  ARRAY['B']),
        (5,  ARRAY['C']),
        (6,  ARRAY['D']),
        (7,  ARRAY['A']),
        (8,  ARRAY['B']),
        (9,  ARRAY['C']),
        (10, ARRAY['B']),
        (11, ARRAY['C']),
        (12, ARRAY['D']),
        (13, ARRAY['E']),
        (14, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_nested_fixedlen
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A (B C){2} D)+ E)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags)
);

-- Doubly nested fixed-length group absorption: (A ((B C{3}){2} D){2} E)+ F
-- step_size = 1 + ((1+3)*2+1)*2 + 1 = 20; 2 iterations + F = 41 rows
WITH test_absorb_doubly_nested AS (
    SELECT v AS id, ARRAY[
        CASE
            WHEN v % 41 IN (1, 21)  THEN 'A'
            WHEN v % 41 IN (2, 6, 11, 15, 22, 26, 31, 35) THEN 'B'
            WHEN v % 41 IN (3,4,5, 7,8,9, 12,13,14, 16,17,18,
                            23,24,25, 27,28,29, 32,33,34, 36,37,38) THEN 'C'
            WHEN v % 41 IN (10, 19, 30, 39) THEN 'D'
            WHEN v % 41 IN (20, 40) THEN 'E'
            WHEN v % 41 = 0 THEN 'F'
            ELSE 'X'
        END
    ] AS flags
    FROM generate_series(1, 82) AS s(v)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_doubly_nested
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A ((B C C C){2} D){2} E)+ F)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags),
        F AS 'F' = ANY(flags)
);

-- 3-level END chain: ((A (B C){2}){2})+
-- Tests END(BC{2}) -> END(A..{2}) -> END(+) chaining
-- 2 iterations of +, each 10 rows: (A B C B C)(A B C B C)
WITH test_absorb_3level_end AS (
    SELECT * FROM (VALUES
        (1,  ARRAY['A']),  -- 1st + iter, 1st {2}, A
        (2,  ARRAY['B']),
        (3,  ARRAY['C']),
        (4,  ARRAY['B']),
        (5,  ARRAY['C']),  -- 1st (BC){2} done
        (6,  ARRAY['A']),  -- 1st + iter, 2nd {2}, A
        (7,  ARRAY['B']),
        (8,  ARRAY['C']),
        (9,  ARRAY['B']),
        (10, ARRAY['C']),  -- 2nd (BC){2} done, 1st {2} done, 1st + iter done
        (11, ARRAY['A']),  -- 2nd + iter, 1st {2}, A
        (12, ARRAY['B']),
        (13, ARRAY['C']),
        (14, ARRAY['B']),
        (15, ARRAY['C']),
        (16, ARRAY['A']),  -- 2nd + iter, 2nd {2}, A
        (17, ARRAY['B']),
        (18, ARRAY['C']),
        (19, ARRAY['B']),
        (20, ARRAY['C']),  -- 2nd + iter done
        (21, ARRAY['X'])   -- no match, + ends
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_absorb_3level_end
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A (B C){2}){2})+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Multiple unbounded: A+ B+ (first element unbounded enables absorption)
WITH test_multi_unbounded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_multi_unbounded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Context Lifecycle
-- ============================================================

-- Multiple overlapping contexts (SKIP TO NEXT ROW)
WITH test_overlapping_contexts AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_overlapping_contexts
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Failed context cleanup (early failure)
WITH test_context_cleanup AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),  -- Pruned at first row
        (2, ARRAY['A']),
        (3, ARRAY['_']),  -- Mismatched after row 2
        (4, ARRAY['A']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_context_cleanup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Partition end (incomplete contexts)
WITH test_partition_end AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
        -- Pattern requires B, but partition ends
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_partition_end
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Completed context encountered during processing
-- Pattern (A | B C D): Ctx1 takes long B->C->D path, while Ctx2 takes
-- short A path and completes first. Next row sees Ctx2
-- with states=NULL and skips it.
WITH test_completed_ctx AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B', '_']),
        (2, ARRAY['C', 'A']),
        (3, ARRAY['D', '_']),
        (4, ARRAY['_', '_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_completed_ctx
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A | B C D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Reluctant context lifecycle (A+? B with SKIP TO NEXT ROW)
-- A+? exits early but if B not available, falls back to loop.
-- Contexts not absorbed (reluctant), so multiple survive.
WITH test_reluctant_context AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_context
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Advance Phase (Epsilon Transitions)
-- ============================================================

-- Nested groups ((A B)+)
WITH test_nested_groups AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_groups
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Multiple alternation branches (A (B|C|D) E)
WITH test_multi_alt AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['E']),
        (4, ARRAY['A']),
        (5, ARRAY['C']),
        (6, ARRAY['E']),
        (7, ARRAY['A']),
        (8, ARRAY['D']),
        (9, ARRAY['E'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_multi_alt
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A (B | C | D) E)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags)
);

-- Optional VAR at start (A? B C)
WITH test_optional_var AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),  -- A skipped
        (2, ARRAY['C']),
        (3, ARRAY['A']),  -- A matched
        (4, ARRAY['B']),
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_optional_var
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A? B C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Nested alternation ((A|B) (C|D))
WITH test_nested_alt AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['C']),  -- A C
        (3, ARRAY['A']),
        (4, ARRAY['D']),  -- A D
        (5, ARRAY['B']),
        (6, ARRAY['C']),  -- B C
        (7, ARRAY['B']),
        (8, ARRAY['D'])   -- B D
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_alt
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B) (C | D))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Mixed greedy/reluctant sequence: A+? B+ (reluctant A, greedy B)
-- A exits as early as possible, B consumes the rest greedily
WITH test_mixed_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_mixed_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+? B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Optional reluctant group: (A B)?? C
-- Reluctant group entry tries skip first, but the skip path needs C
-- at row 1 which is A -> skip fails. Enter path succeeds: A(1) B(2) C(3).
WITH test_optional_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_optional_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)?? C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Non-leading reluctant optional VAR: (B A?? C)
-- Reluctant A?? should prefer to skip, matching B(1) C(2) with A left
-- unmatched (match_end 2).  The leading/group reluctant cases above go through
-- the begin path; this exercises the non-leading skip path,
-- which must honor reluctant ordering too.
WITH test_nonleading_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A', 'C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nonleading_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (B A?? C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Reluctant outer quantifier over a nullable reluctant body: SQL/RPR
-- semantics call for the shortest (empty) match.  In the count<min case
-- the engine must prefer the fast-forward (exit) path for reluctant
-- groups and suppress longer matches once exit reaches FIN, mirroring the
-- sibling min<=count<max branch.  The 2-level greedy/reluctant matrix plus a
-- min>=2 boundary and single-quantifier controls localize the behaviour: the
-- inner quantifier decides whether a row is consumed, so every column whose
-- body is reluctant stays at zero, and the two with a greedy body differ by
-- their outer quantifier -- gg takes the longest match, rg one row.
WITH t(id, isa) AS (VALUES (1, true), (2, true), (3, true), (4, false))
SELECT id,
       count(*) OVER gg  AS gg,     -- (A?)+      greedy / greedy
       count(*) OVER gr  AS gr,     -- (A??)+     greedy / reluctant
       count(*) OVER rg  AS rg,     -- (A?)+?     reluctant / greedy
       count(*) OVER rr  AS rr,     -- (A??)+?    reluctant / reluctant
       count(*) OVER rr2 AS rr2,    -- (A??){2,}? reluctant, min>=2 boundary
       count(*) OVER ca  AS ca,     -- A??        single reluctant control
       count(*) OVER cs  AS cs      -- A*?        single reluctant control
FROM t
WINDOW gg  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A?)+)      DEFINE A AS isa),
       gr  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A??)+)     DEFINE A AS isa),
       rg  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A?)+?)     DEFINE A AS isa),
       rr  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A??)+?)    DEFINE A AS isa),
       rr2 AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN ((A??){2,}?) DEFINE A AS isa),
       ca  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A??)        DEFINE A AS isa),
       cs  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING PATTERN (A*?)        DEFINE A AS isa)
ORDER BY id;

-- Doubly-nested reluctant nullable group: (((A??){2,}?){2,}?).  Reluctant
-- quantifiers disable optimizer flattening, so both levels survive and the
-- inner group's END->next lands on the outer END.  This exercises the
-- END->END count increment in the EMPTY_LOOP fast-forward (count < min).
WITH t(id, isa) AS (VALUES (1, true), (2, true), (3, false))
SELECT id, count(*) OVER w AS c
FROM t
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (((A??){2,}?){2,}?)
    DEFINE A AS isa
)
ORDER BY id;

-- Non-leading reluctant optional GROUP with a follower: (B (A X)?? C)
-- Like the VAR case above but a multi-element group; it goes through the
-- begin path, which already honors reluctant ordering.
-- Reluctant (A X)?? should skip, matching B(1) C(2), with the group skipped
-- to the following C (not to FIN).
WITH test_nonleading_reluctant_group AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A', 'C']),
        (3, ARRAY['X']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nonleading_reluctant_group
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (B (A X)?? C)
    DEFINE
        A AS 'A' = ANY(flags),
        X AS 'X' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Reluctant nullable group with a required follower: ((A??){2,}? B).
-- min=2 forces the reluctant fast-forward to loop back (it cannot exit to
-- FIN until B matches), exercising the loop-back route_to_elem second call
-- site in nfa_advance_end.  B fails at the group's exit row, so only the
-- first match survives.
WITH test_reluctant_nullable_follower AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_nullable_follower
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A??){2,}? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Greedy/reluctant sequence: A+ B+? (greedy A, reluctant B at end)
-- A consumes greedily, B+? exits to FIN after minimum match
WITH test_greedy_then_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_greedy_then_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+?)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant optional group skip-to-FIN
-- When a reluctant optional group's skip path reaches FIN, the group
-- entry path is abandoned.
-- Pattern: C (A B)?? -- after C matches, the reluctant group (A B)??
-- prefers to skip.  Skip goes to FIN (group is last element), so
-- the match completes with just C.
WITH test_begin_skip_fin AS (
    SELECT * FROM (VALUES
        (1, ARRAY['C']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['C','A']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_begin_skip_fin
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (C (A B)??)
    DEFINE
        C AS 'C' = ANY(flags),
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Match Phase
-- ============================================================

-- Simple VAR with END next (A B C all min=max=1)
WITH test_simple_var AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_simple_var
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- VAR max exceeded (A{2,3})
WITH test_max_exceeded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),  -- Max = 3
        (4, ARRAY['A']),  -- Exceeds max, state removed
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_max_exceeded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{2,3} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Non-matching VAR (DEFINE false)
WITH test_non_matching AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['_']),  -- B not matched (DEFINE false)
        (3, ARRAY['A']),
        (4, ARRAY['B']),  -- B matched
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_non_matching
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ============================================================
-- Frame Boundary Handling
-- ============================================================

-- Limited frame (ROWS BETWEEN CURRENT ROW AND 3 FOLLOWING)
WITH test_limited_frame AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),  -- Within 3 FOLLOWING
        (5, ARRAY['B']),  -- Beyond 3 FOLLOWING from row 1
        (6, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_limited_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 3 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Unbounded frame (ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING)
WITH test_unbounded_frame AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])  -- Far from start, but unbounded
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_unbounded_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Match exceeds frame boundary
WITH test_frame_exceeded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
        -- Frame ends at row 3 (2 FOLLOWING), B never appears
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_frame_exceeded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Frame boundary forced mismatch
-- Limited frame with enough rows so that a context's frame boundary
-- is exceeded while still processing.
WITH test_frame_boundary AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_frame_boundary
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant with limited frame (A+? B with 2 FOLLOWING)
-- Reluctant exits early, B must be within frame boundary
WITH test_reluctant_frame AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_frame
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- State Management
-- ============================================================

-- Duplicate state creation
WITH test_duplicate_states AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),  -- Both A and B match (creates duplicate states via different paths)
        (2, ARRAY['C', '_']),
        (3, ARRAY['D', '_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_duplicate_states
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B) C D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Reluctant duplicate state handling
-- (A+? | B+?) creates exit and loop states; exit paths may converge
WITH test_reluctant_dedup AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_dedup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+? | B+?))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Large pattern (stress free list)
WITH test_large_pattern AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['E']),
        (6, ARRAY['F']),
        (7, ARRAY['G']),
        (8, ARRAY['H']),
        (9, ARRAY['I']),
        (10, ARRAY['J'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_large_pattern
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C D E F G H I J)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags),
        F AS 'F' = ANY(flags),
        G AS 'G' = ANY(flags),
        H AS 'H' = ANY(flags),
        I AS 'I' = ANY(flags),
        J AS 'J' = ANY(flags)
);

-- Reduced frame map reallocation (> 1024 rows)
WITH test_map_realloc AS (
    SELECT id, CASE WHEN id % 2 = 1 THEN ARRAY['A'] ELSE ARRAY['B'] END AS flags
    FROM generate_series(1, 1100) AS id
)
SELECT count(*), min(match_start), max(match_end)
FROM (
    SELECT id, flags,
           first_value(id) OVER w AS match_start,
           last_value(id) OVER w AS match_end
    FROM test_map_realloc
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP TO NEXT ROW
        PATTERN (A B)
        DEFINE
            A AS 'A' = ANY(flags),
            B AS 'B' = ANY(flags)
    )
) sub;

-- ============================================================
-- Statistics and Diagnostics
-- ============================================================

-- Matched contexts
WITH test_matched AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_matched
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Pruned contexts (failed at first row)
WITH test_pruned AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),  -- Pruned
        (2, ARRAY['_']),  -- Pruned
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_pruned
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Mismatched contexts (failed after multiple rows)
WITH test_mismatched AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['_']),  -- Mismatched after 2 rows
        (4, ARRAY['A']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_mismatched
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant not absorbed (A+? with SKIP TO NEXT ROW)
-- Compare with greedy A+ below: reluctant is not absorbable,
-- so all contexts survive independently.
WITH test_reluctant_stats AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_stats
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+?)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Absorbed contexts
WITH test_absorbed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_absorbed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Skipped contexts (SKIP TO NEXT ROW)
WITH test_skipped AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])  -- Completes match starting at row 1
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skipped
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Quantifier Runtime Behavior
-- ============================================================

-- Large count handling (A{100})
WITH test_large_count AS (
    SELECT i AS id, ARRAY['A'] AS flags
    FROM generate_series(1, 105) i
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_large_count
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{100})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Unlimited quantifier (A{10,})
WITH test_unlimited AS (
    SELECT i AS id, ARRAY['A'] AS flags
    FROM generate_series(1, 15) i
    UNION ALL
    SELECT 16, ARRAY['B']
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_unlimited
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{10,} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Min boundary (A{3,5})
WITH test_min_boundary AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),  -- Min=3 reached, exit path available
        (4, ARRAY['B']),  -- Match ends at min
        (5, ARRAY['A']),
        (6, ARRAY['A']),
        (7, ARRAY['A']),
        (8, ARRAY['A']),
        (9, ARRAY['A']),  -- Count=5, max reached
        (10, ARRAY['B'])  -- Match ends at max
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_min_boundary
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{3,5} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Max boundary exceeded (A{3,5})
WITH test_max_boundary AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['A']),  -- Count=6 > max=5, row 1 context removed
        (7, ARRAY['B'])   -- Row 1 context: no match (exceeded max)
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_max_boundary
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A{3,5} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Greedy vs reluctant: A+ matches all rows, A+? matches minimum
WITH test_greedy_vs_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','_']),
        (2, ARRAY['A','_']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B','_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_greedy_vs_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Same data, reluctant A+? exits at row 3 where B is first available
WITH test_greedy_vs_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','_']),
        (2, ARRAY['A','_']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B','_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_greedy_vs_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant group: (A B)+? matches minimum 1 iteration
WITH test_reluctant_group AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_group
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+?)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A+? B (reluctant plus): exits A at first B availability
-- (Standalone reluctant-plus case; compare with A{1,3}? and A{3,5}? below)
WITH test_reluctant_plus AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','_']),
        (2, ARRAY['A','_']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B','_'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_reluctant_plus
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A{1,3}? B (reluctant bounded): same data, bounded quantifier
WITH test_reluctant_bounded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','_']),
        (2, ARRAY['A','_']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B','_'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_reluctant_bounded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{1,3}? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A{3,5}? B (reluctant bounded mid-band): the VAR-level count
-- cycles through 3, 4, 5 within a single match attempt.  Exercises
-- a reluctant bounded quantifier that absorbability analysis excludes
-- (reluctant quantifiers are never absorbable, so A stays
-- non-absorbable).
WITH test_reluctant_mid_band AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_mid_band
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{3,5}? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested quantifier flattening must not widen the matching language.
-- (A{k,})* with k >= 2 reaches repetition counts {0} UNION [k, INF); the gap
-- 1..k-1 is unreachable, so it must NOT collapse to A*.  An isolated single A
-- must yield an EMPTY match (count 0), not a length-1 match.
WITH test_nested_quant_var AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),  -- isolated A: (A{2,})* matches empty here, not 1
        (2, ARRAY['_']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),  -- run of 2: matched
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end,
       count(*) OVER w AS match_count
FROM test_nested_quant_var
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A{2,})*)
    DEFINE A AS 'A' = ANY(flags)
);

-- Same for a GROUP child: ((A B){2,})* must not collapse to (A B)*.
-- An isolated single (A B) pair must yield an EMPTY match (count 0).
WITH test_nested_quant_group AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),  -- isolated (A B) pair: matches empty here
        (2, ARRAY['B']),
        (3, ARRAY['_']),
        (4, ARRAY['A']),
        (5, ARRAY['B']),
        (6, ARRAY['A']),
        (7, ARRAY['B']),  -- run of 2 pairs: matched
        (8, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end,
       count(*) OVER w AS match_count
FROM test_nested_quant_group
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A B){2,})*)
    DEFINE A AS 'A' = ANY(flags), B AS 'B' = ANY(flags)
);

-- Optional VAR at the head of the body, re-entered across iterations:
-- (B? A+){2} with no B rows.  Both iterations skip B and re-enter A.
WITH test_optvar_quant_reentry AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_optvar_quant_reentry
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((B? A+){2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A skip path that lands on the group's END still counts that iteration:
-- (UP DOWN?)+ over a rising run, where DOWN never matches.
WITH test_skip_lands_on_end AS (
    SELECT * FROM (VALUES
        (1, 100),
        (2, 110),
        (3, 120)
    ) AS t(id, price)
)
SELECT id, price,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_lands_on_end
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((UP DOWN?)+)
    DEFINE
        UP AS price > PREV(price),
        DOWN AS price < PREV(price)
);

-- Deep giveback: A+ swallows every overlapping row, then B{3} fails and
-- A+ must surrender three rows in a row.  The failure is discovered five
-- rows away from the branch point, not adjacent to it.
WITH test_quant_deep_giveback AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['A','B']),
        (5, ARRAY['A','B']),
        (6, ARRAY['A','B']),
        (7, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_deep_giveback
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B{3} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- A+ B+ A+ over rows that are all A and B: three quantifiers draw from one
-- pool, so shrinking the first changes what the other two can take.  They
-- must renegotiate jointly, not one at a time.
WITH test_quant_coupled_spans AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['A','B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_coupled_spans
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B+ A+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Pathological Pattern Runtime Protection
-- ============================================================

-- Complex nested nullable ((A* B*)*) - Runtime protection
WITH test_complex_nested AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B']),
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_complex_nested
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A* B*)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested nullable with quantifier ((A{0,3})*)
WITH test_nested_quantifier AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_quantifier
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A{0,3})*)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Reluctant nullable: A*? (prefers 0 matches)
-- A*? always takes skip path (0 iterations preferred)
WITH test_reluctant_nullable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_nullable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A*? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Consecutive groups are merged only when the body has a fixed row count.
-- (A | B B)+ (A | B B)+ keeps both groups: the merged (A | B B){2,} would
-- stop after two rows, because two iterations already meet its lower bound,
-- while the second group here still demands an iteration of its own.
WITH test_group_merge_uneven_alt AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A', 'B']),
        (3, ARRAY['B']),
        (4, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_group_merge_uneven_alt
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B B)+ (A | B B)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Same guard, reached through a reluctant quantifier in the body: B A+? has no
-- fixed row count either, so (B A+?)+ (B A+?)+ is left alone.
WITH test_group_merge_reluctant_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_group_merge_reluctant_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((B A+?)+ (B A+?)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- The trailing copy in (A | B B){1,2} (A | B B) is not folded into the group
-- for the same reason.  A leading copy would be, since it is mandatory.
WITH test_suffix_merge_uneven_alt AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A', 'B']),
        (3, ARRAY['B']),
        (4, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_suffix_merge_uneven_alt
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B B){1,2} (A | B B))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested bounded quantifiers ((A{2,3}){1,2}) are not flattened into A{2,6}:
-- the first iteration's count is settled before the group decides whether to
-- iterate again, so with four A rows it takes three and leaves the group
-- rather than taking two and two.  A{2,6} would take all four.
WITH test_nested_bounded_quantifier AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_bounded_quantifier
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A{2,3}){1,2})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- ============================================================
-- Alternation Runtime Behavior
-- ============================================================

-- Multi-branch alternation (A (B|C|D|E) F)
WITH test_multi_branch AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['F']),
        (4, ARRAY['A']),
        (5, ARRAY['C']),
        (6, ARRAY['F']),
        (7, ARRAY['A']),
        (8, ARRAY['D']),
        (9, ARRAY['F']),
        (10, ARRAY['A']),
        (11, ARRAY['E']),
        (12, ARRAY['F'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_multi_branch
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A (B | C | D | E) F)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags),
        F AS 'F' = ANY(flags)
);

-- Alternation with quantifiers (A+ | B+ | C+)
WITH test_alt_quantifiers AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['B']),
        (6, ARRAY['C']),
        (7, ARRAY['C']),
        (8, ARRAY['C']),
        (9, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_quantifiers
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ | B+ | C+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Branch preference (A B C | D): D completes first at row 1, but the
-- earlier-written A B C branch is preferred and replaces the match at row 3.
WITH test_alt_replace AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'D']),
        (2, ARRAY['B', '_']),
        (3, ARRAY['C', '_']),
        (4, ARRAY['_', '_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_replace
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C | D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ALT lexical order takes priority over greedy (longer match).
-- Row 1 matches both A and B; A wins by lexical order (match 1-1).
WITH test_alt_lexical_order AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),  -- A and B both match
        (2, ARRAY['_','C'])   -- only C matches (would continue B C)
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_lexical_order
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B C)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Preference beats length even when the preferred branch matches empty: the
-- A* branch reaches FIN with no rows consumed and nfa_advance_alt stops there,
-- so B is never expanded on a row where it would have matched.
WITH test_alt_empty_pref AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),
        (2, ARRAY['_', 'B']),
        (3, ARRAY['A', 'B']),
        (4, ARRAY['_', 'B'])
    ) AS t(id, flags)
)
SELECT id, flags, count(*) OVER w AS n
FROM test_alt_empty_pref
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A* | B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Swapping the branches makes B preferred, so every row matches one row.
WITH test_alt_empty_pref AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),
        (2, ARRAY['_', 'B']),
        (3, ARRAY['A', 'B']),
        (4, ARRAY['_', 'B'])
    ) AS t(id, flags)
)
SELECT id, flags, count(*) OVER w AS n
FROM test_alt_empty_pref
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (B | A*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ALT with reluctant: (A+? | B+) - A branch is reluctant, B is greedy.
-- Row 1 matches both A and B. A+? exits immediately (match 1-1).
WITH test_alt_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['B','_']),
        (3, ARRAY['B','_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+? | B+))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Optional first branch in ALT with quantifier: (A? | B){1,2}
-- First branch A? exit path may loop back to ALT and trigger cycle
-- detection during DFS.  On a B row the A? branch still succeeds with an
-- empty match, which outranks the B branch, so the match is empty.
WITH test_alt_opt_first AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_opt_first
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A? | B){1,2}))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Mixed A/B rows across iterations of (A? | B){1,2}
WITH test_alt_opt_mixed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A','B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_opt_mixed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A? | B){1,2}))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant variant: (A?? | B){1,2}
WITH test_alt_opt_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_opt_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A?? | B){1,2}))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Overlapping match: A B C D E | B C D | C D E F (SKIP PAST LAST ROW)
WITH test_overlap1 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['E']),
        (6, ARRAY['F'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_overlap1
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E | B C D | C D E F)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags),
        F AS 'F' = ANY(flags)
);

-- Same with SKIP TO NEXT ROW: three overlapping matches
WITH test_overlap1 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['E']),
        (6, ARRAY['F'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_overlap1
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C D E | B C D | C D E F)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags),
        F AS 'F' = ANY(flags)
);

-- Longer pattern fails, shorter survives: A+ B C D E | B+ C
WITH test_overlap1b AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_overlap1b
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B C D E | B+ C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags)
);

-- Long B sequence with different endings: A B+ C | B+ D
WITH test_overlap2 AS (
    SELECT * FROM (VALUES
        (1,  ARRAY['A']),
        (2,  ARRAY['B']),
        (3,  ARRAY['B']),
        (4,  ARRAY['B']),
        (5,  ARRAY['B']),
        (6,  ARRAY['C']),
        (7,  ARRAY['B']),
        (8,  ARRAY['B']),
        (9,  ARRAY['B']),
        (10, ARRAY['D'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_overlap2
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B+ C | B+ D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Greedy with late failure ("betrayal"): A B C+ D | A B
WITH test_betrayal AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['C']),
        (5, ARRAY['C']),
        (6, ARRAY['E'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_betrayal
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C+ D | A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Multiple TRUE per row: overlapping pattern variables
WITH test_multi_true AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['B','C']),
        (3, ARRAY['C','D']),
        (4, ARRAY['D','E']),
        (5, ARRAY['E','_'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_multi_true
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B C D E)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags)
);

-- Diagonal pattern with shifted multi-TRUE overlap
WITH test_diagonal AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','_']),
        (2, ARRAY['B','A']),
        (3, ARRAY['C','B']),
        (4, ARRAY['D','C']),
        (5, ARRAY['_','D'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_diagonal
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ((A | B) C)+ - alternation inside group with outer quantifier
WITH test_alt_group AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['C']),
        (3, ARRAY['B']),
        (4, ARRAY['C']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_alt_group
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B) C)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A | (B C)+ (D E)+): the last branch is (B C)+ (D E)+, so with no A/B/C the
-- rows D E D E match nothing -- the trailing (D E)+ is not its own branch.
WITH test_alt_concat_groups AS (
    SELECT * FROM (VALUES
        (1, ARRAY['D']),
        (2, ARRAY['E']),
        (3, ARRAY['D']),
        (4, ARRAY['E'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_concat_groups
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A | (B C)+ (D E)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags),
        E AS 'E' = ANY(flags)
);

-- (A | (B C)*): optional group as the last ALT branch.  When (B C)* matches
-- zero the branch ends with an empty match (NULL bounds), consuming no row.
WITH test_alt_tail_optgroup AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_tail_optgroup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A | (B C)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ((B C)* | A): optional group as a non-last branch.  Where B C does start
-- the group consumes it (row 2); elsewhere the branch still succeeds with a
-- zero match, and that outranks the A branch, so A never fires (row 4).
WITH test_alt_nonlast_optgroup AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_nonlast_optgroup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((B C)* | A)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (((B C)* | A) D): the ALT is followed by D, so a zero-match (B C)* must
-- continue into D.  Row 4 (a lone D) exercises that; row 1 is the B C D match.
WITH test_alt_optgroup_then_elem AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['C']),
        (3, ARRAY['D']),
        (4, ARRAY['D']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_optgroup_then_elem
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((B C)* | A) D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ((B C)* D | A): the same group inside a non-last branch, with D following it
-- in that branch.  A zero-match (B C)* must continue into D, not fall through
-- into the A branch; row 4 (a lone D) is the row that tells the two apart.
WITH test_alt_nonlast_grp_then_elem AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['C']),
        (3, ARRAY['D']),
        (4, ARRAY['D']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_nonlast_grp_then_elem
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((B C)* D | A)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ((B C)*? | A): a reluctant optional group as a branch prefers zero
-- iterations, so the first branch matches empty on every row and the A
-- branch never fires.
WITH test_alt_reluctant_optgroup AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['C']),
        (3, ARRAY['A']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_reluctant_optgroup
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((B C)*? | A)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Same variable re-entered across iterations: (A+ | B){2} on an all-A
-- partition.  Both iterations take the A+ branch, so the second one re-enters
-- VAR A with a higher iteration count -- a new state, not a cycle.
WITH test_alt_quant_reentry AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_quant_reentry
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A+ | B){2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Branch order does not change the match: (B | A+){2} is the same.
WITH test_alt_quant_reentry_order AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_quant_reentry_order
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((B | A+){2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Hand-unrolled form.  mergeConsecutiveAlts rolls it back up to {2}, so the
-- match must be the same as the two above.
WITH test_alt_quant_unrolled AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_quant_unrolled
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    INITIAL
    PATTERN ((A+ | B) (A+ | B))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ((A B) | (C D))+ with both branches two rows wide and every row shared:
-- the branch taken at one iteration decides which rows remain for the next.
WITH test_alt_multirow_branches AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','C']),
        (2, ARRAY['B','D']),
        (3, ARRAY['A','C']),
        (4, ARRAY['B','D']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_multirow_branches
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A B) | (C D))+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Same rows, branches written in the other order.  Both branches match the
-- same rows here, so the result must not change: the classification does,
-- but the frame does not.
WITH test_alt_multirow_swapped AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','C']),
        (2, ARRAY['B','D']),
        (3, ARRAY['A','C']),
        (4, ARRAY['B','D']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_multirow_swapped
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((C D) | (A B))+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ============================================================
-- Deep Nested Groups
-- ============================================================

-- Three-level nesting ((((A B)+)+)+)
WITH test_deep_nesting AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_deep_nesting
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((((A B)+)+)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Multiple groups in nesting (((A B) (C D))+)
WITH test_nested_sequential AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['D']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['C']),
        (8, ARRAY['D']),
        (9, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_sequential
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A B) (C D))+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Nested END->END max reached
-- Inner group (A B){2} reaches max=2 -> exits to outer END
WITH test_end_nested_max AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['A']),
        (8, ARRAY['B']),
        (9, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_end_nested_max
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A B){2})+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested END->END between min/max
-- Inner group (A B){1,3} exits between min/max -> outer END count++
WITH test_end_nested_mid AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B']),
        (7, ARRAY['A']),
        (8, ARRAY['B']),
        (9, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_end_nested_mid
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A B){1,3})+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested reluctant group ((A B)+?) with following element C
-- Inner group exits after minimum 1 iteration
WITH test_nested_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B)+? C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A B){2} - group with exact quantifier
WITH test_group_exact AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['X'])
    ) AS t(id, flags)
)
SELECT id, flags, first_value(id) OVER w AS match_start, last_value(id) OVER w AS match_end
FROM test_group_exact
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B){2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Nested END->END fast-forward
-- When an inner group has a nullable body and count < min, the
-- fast-forward path exits through the outer END, incrementing
-- the outer group's count.
-- Pattern: ((A?){2,3}){2,3} -- nested groups, neither collapses
-- because the optimizer cannot safely multiply non-exact quantifiers.
-- Data has no A rows, forcing all-empty iterations via fast-forward.
WITH test_nested_ff AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_ff
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A?){2,3}){2,3})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Exact outer quantifier over a variable-length body
-- (X{1,2}){2} is by definition X{1,2} X{1,2}, so the two must prefer the same
-- match.  Collapsing to (A | B B){2,4} does not: the branches consume unequal
-- rows, so moving an iteration across the block boundary changes which rows
-- match.  The third query below shows the collapsed form's own preference.
WITH test_nested_alt_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_alt_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B B){1,2}){2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- The same pattern written out.  Must give the same match as the nested form.
WITH test_nested_alt_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_alt_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B B){1,2} (A | B B){1,2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- The collapsed form, written by hand: same iteration counts as the two above,
-- but with no block boundary forcing a second iteration it stops at rows 1-2.
-- That difference is why the two above must not be rewritten into this one.
WITH test_nested_alt_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nested_alt_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B B){2,4})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Re-entry into a group whose body always consumes rows
-- A body that cannot match empty consumes a row on every derivation, so a
-- loop-back into it is progress, not a cycle.  Both forms must find the same
-- match: (X){3} is by definition X X X.
WITH test_reentry_consuming AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['A','B']),
        (7, ARRAY['B']),
        (8, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reentry_consuming
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B B){1,3}){3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- The same pattern written out.  Must give the same match as the nested form.
WITH test_reentry_consuming AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['A','B']),
        (7, ARRAY['B']),
        (8, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reentry_consuming
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B B){1,3} (A | B B){1,3} (A | B B){1,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Empty iteration followed by a consuming one, below min
-- A? is tried before B, so on row 1 the first two iterations go empty and the
-- third takes B, matching rows 1-2.  The longer A B C match ranks lower: it
-- abandons A? in the first iteration (7.2.4 -- length breaks prefix ties only).
WITH test_empty_then_consume AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A','C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_then_consume
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A? | B){3} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- The same pattern written out, with the copies renamed so that no rewrite
-- can fold them back into a loop.  Must give the same match.
WITH test_empty_then_consume AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A','C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_then_consume
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A? | B) (D? | E) (F? | G) C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'A' = ANY(flags),
        E AS 'B' = ANY(flags),
        F AS 'A' = ANY(flags),
        G AS 'B' = ANY(flags)
);

-- Bound above the empty-then-consume cases above.  The rows are the same at
-- every bound from 2 up while the derivation shifts, so a shortcut through
-- the below-min counts first shows here, where they are not adjacent.
WITH test_empty_then_consume_bound AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A']),
        (3, ARRAY['A','C']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_then_consume_bound
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A? | B){4} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Recursion depth of the same below-min path: the fall-through advances the
-- count by one empty iteration at a time, so the depth follows the bound.
-- Reduced stack so the limit is reached quickly.
SET max_stack_depth = '100kB';
WITH test_below_min_depth AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A','C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_below_min_depth
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A? | B){10000} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);
RESET max_stack_depth;

-- (A+)+ B with an overlap row: the inner A+ must take row 3 as A, leaving
-- row 4 for B.  Nested unbounded quantifiers over a row that satisfies both
-- the quantified variable and its successor.
WITH test_nest_unbounded_overlap AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nest_unbounded_overlap
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+)+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ((A+ B)+ C)+ D: three unbounded quantifiers at three depths, every row
-- shared with its successor.  Iteration boundaries at each level can be
-- drawn several ways over the same rows.
WITH test_nest_three_levels AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B','C']),
        (4, ARRAY['C','D']),
        (5, ARRAY['D'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nest_three_levels
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A+ B)+ C)+ D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ((A | B)+)+ C: alternation nested inside two unbounded quantifiers, with
-- rows satisfying both alternatives.
WITH test_nest_alt_unbounded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B','C']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_nest_alt_unbounded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (((A | B)+)+ C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ============================================================
-- SKIP Options (Runtime)
-- ============================================================

-- SKIP PAST LAST ROW (non-overlapping matches)
WITH test_skip_past AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_past
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- SKIP TO NEXT ROW (overlapping matches)
WITH test_skip_next AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_next
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- SKIP difference verification
WITH test_skip_diff AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT 'SKIP PAST' AS mode, id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_diff
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
)
UNION ALL
SELECT 'SKIP NEXT' AS mode, id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_diff
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
)
ORDER BY mode, id;

-- Reluctant SKIP comparison: A+? with SKIP PAST vs SKIP NEXT
WITH test_reluctant_skip AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT 'SKIP PAST' AS mode, id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_skip
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+?)
    DEFINE
        A AS 'A' = ANY(flags)
)
UNION ALL
SELECT 'SKIP NEXT' AS mode, id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_skip
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+?)
    DEFINE
        A AS 'A' = ANY(flags)
)
ORDER BY mode, id;

-- A* under SKIP PAST LAST ROW: row 1 matches empty, which consumes nothing
-- and so must not move the skip landing.  Row 2 still starts its own match.
WITH test_skip_past_after_empty AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_past_after_empty
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A*)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- (A B)* C: row 1 matches C alone with the group taken zero times, so the
-- landing is row 2 and the A B C match at rows 2-4 is still found.
WITH test_skip_past_zero_iterations AS (
    SELECT * FROM (VALUES
        (1, ARRAY['C']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_skip_past_zero_iterations
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)* C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ============================================================
-- INITIAL Mode (Runtime)
-- ============================================================

-- Explicit INITIAL (after AFTER MATCH SKIP, per the grammar); same as the default
WITH test_initial_mode AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),  -- Unmatched
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['_']),  -- Unmatched
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_initial_mode
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    INITIAL
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Default mode (include all rows)
WITH test_default_mode AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),  -- Unmatched, but included
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['_']),  -- Unmatched, but included
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_default_mode
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Mode equivalence verification: explicit INITIAL equals the default mode
WITH test_mode_diff AS (
    SELECT * FROM (VALUES
        (1, ARRAY['_']),
        (2, ARRAY['A']),
        (3, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT 'INITIAL' AS mode, COUNT(*) AS row_count
FROM (
    SELECT id FROM test_mode_diff
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP TO NEXT ROW
        INITIAL
        PATTERN (A)
        DEFINE A AS 'A' = ANY(flags)
    )
) sub
UNION ALL
SELECT 'DEFAULT' AS mode, COUNT(*) AS row_count
FROM (
    SELECT id FROM test_mode_diff
    WINDOW w AS (
        ORDER BY id
        ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
        AFTER MATCH SKIP TO NEXT ROW
        PATTERN (A)
        DEFINE A AS 'A' = ANY(flags)
    )
) sub
ORDER BY mode;

-- ============================================================
-- Frame Boundary Variations
-- ============================================================

-- Very limited frame (1 FOLLOWING)
WITH test_one_following AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),  -- Within 1 FOLLOWING
        (3, ARRAY['A']),  -- Beyond 1 FOLLOWING from row 1
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_one_following
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 1 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Medium frame (10 FOLLOWING)
WITH test_ten_following AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A']),
        (6, ARRAY['A']),
        (7, ARRAY['A']),
        (8, ARRAY['A']),
        (9, ARRAY['A']),
        (10, ARRAY['A']),
        (11, ARRAY['B']),  -- Within 10 FOLLOWING from row 1
        (12, ARRAY['A']),
        (13, ARRAY['B'])   -- Beyond 10 FOLLOWING from row 1
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_ten_following
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 10 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Exact boundary match
WITH test_exact_boundary AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['B'])   -- Exactly at 4 FOLLOWING (frame end)
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_exact_boundary
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 4 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- N FOLLOWING + SKIP TO NEXT ROW: overlapping matches bounded by frame
WITH test_n_skip_next AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_n_skip_next
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 3 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Frame exactly 1 row short of potential match
-- From row 1: A A A B needs 4 rows but frame holds 3 -> no match
-- From row 2: A A B fits in 3-row frame -> match
WITH test_frame_one_short AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_frame_one_short
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND 2 FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Special Partition Cases
-- ============================================================

-- Empty partition (0 rows)
WITH test_empty_partition AS (
    SELECT * FROM (VALUES
        (1, 1, ARRAY['A']),
        (2, 2, ARRAY['_'])  -- Different partition
    ) AS t(id, part, flags)
)
SELECT id, part, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_partition
WHERE part = 99  -- No rows match
WINDOW w AS (
    PARTITION BY part
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Single row partition
WITH test_single_row AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_single_row
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- All rows fail matching (all DEFINE false)
WITH test_all_fail AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_all_fail
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A+)
    DEFINE
        A AS false  -- All rows fail
);

-- Partition end with absorbable pattern
-- SKIP PAST LAST ROW + unbounded frame + all rows match A
-- Triggers absorb in !rowExists path at partition boundary.
WITH test_absorb_partition_end AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['A']),
        (5, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_absorb_partition_end
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- ============================================================
-- DEFINE Special Cases
-- ============================================================

-- Undefined variable in DEFINE
WITH test_undefined_var AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['X']),  -- B not defined, defaults to TRUE
        (3, ARRAY['C']),
        (4, ARRAY['A']),
        (5, ARRAY['_']),  -- B defaults to TRUE, but no flags
        (6, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_undefined_var
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN (A B C)
    DEFINE
        A AS 'A' = ANY(flags),
        -- B is undefined, defaults to TRUE
        C AS 'C' = ANY(flags)
);

-- ============================================================
-- Absorption Dynamic Flags
-- ============================================================

-- Partial absorbable pattern ((A+) B)
WITH test_partial_absorbable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_partial_absorbable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+) B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Dynamic flag update ((A+) | B)
WITH test_dynamic_flags AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_dynamic_flags
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+) | B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Non-absorbable context during absorption
-- Pattern (A B)+ C: A,B in absorbable group, C is not.
-- When END exits to C, the cloned context becomes non-absorbable.
WITH test_non_absorbable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['C']),
        (6, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_non_absorbable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Absorption skipped when no absorbable state remains
-- Pattern (A B)+ C D with SKIP PAST LAST ROW
-- After reaching C (non-absorbable), no absorbable state remains.
-- On next row (D), the early return fires.
WITH test_absorption_early_return AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['C']),
        (6, ARRAY['D']),
        (7, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_absorption_early_return
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A B)+ C D)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- Coverage failure: older can't cover newer's states
-- Pattern A+ | B+ with SKIP PAST LAST ROW.
-- Row 1: only A -> Ctx1 takes A branch only (B fails).
-- Row 2: A and B -> Ctx2 takes both branches.
-- Absorption: Ctx1 has A but no B -> can't cover Ctx2's B state -> fails.
WITH test_coverage_fail AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', '_']),
        (2, ARRAY['A', 'B']),
        (3, ARRAY['A', '_']),
        (4, ARRAY['A', '_']),
        (5, ARRAY['_', '_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_coverage_fail
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ | B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Absorb skips completed context (older->states==NULL)
-- Pattern A+ | B+ with SKIP PAST LAST ROW.
-- Row 1: A only -> Ctx1 takes A branch. Row 2: B only -> Ctx1 A fails (completed).
-- Ctx2 takes B branch. Absorption: Ctx1 states==NULL -> skip.
WITH test_older_completed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['B']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_older_completed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ | B+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Absorb skips a context with no absorbable state
-- Pattern A+ | B C with SKIP PAST LAST ROW (only A+ branch absorbable).
-- Row 1: B only -> Ctx1 takes B branch (non-absorbable), advances to C.
-- Row 2: C,A -> Ctx1 C matches (no absorbable state). Ctx2 takes A (absorbable).
-- Absorption: Ctx1 has no absorbable state -> skip.
WITH test_older_non_absorbable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B', '_']),
        (2, ARRAY['C', 'A']),
        (3, ARRAY['_', 'A']),
        (4, ARRAY['_', '_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_older_non_absorbable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ | B C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- Reluctant branch in ALT not absorbable: (A+?) | B
-- A+? is reluctant so not absorbable. Compare with greedy (A+) | B above.
WITH test_reluctant_alt_absorption AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_reluctant_alt_absorption
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+?) | B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Zero-Consumption Cycle Detection
-- ============================================================

-- Cycle prevention at count > 0: (A*)* inner skip cycles at count=3
WITH test_cycle_nonzero AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])  -- Inner A* matches 0, cycles at count=3
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_nonzero
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A*)*)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- Cycle with mixed nullables: (A* B*)* multiple nullable paths
WITH test_cycle_mixed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_mixed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A* B*)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A (B*?)+?)+ : a reluctant unbounded quantifier over a nullable group,
-- inside an outer quantifier.  The empty iteration ends the inner loop.
WITH test_cycle_reluctant_nullable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_reluctant_nullable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+?)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Same body under a bounded outer quantifier with lower bound zero.
WITH test_cycle_reluctant_bounded AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_reluctant_bounded
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+?){0,2})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Greedy inner over the same nullable group: the preferred path consumes a
-- row and parks in the frontier, so it cannot recurse through epsilon.
WITH test_cycle_greedy_nullable AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_greedy_nullable
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+){2,})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Non-nullable inner body: every derivation consumes a row, so no empty
-- iteration exists and the guard has nothing to do.
WITH test_cycle_nonnullable_inner AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_nonnullable_inner
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B+)+?){2,})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Reluctant unbounded quantifier over a nullable group, inside an outer
-- quantifier whose lower bound is above one.  Below the bound the guard
-- must keep looping without unbounded epsilon recursion: each empty inner
-- iteration advances the count until the bound is met.  Two iterations
-- (one A each, inner empty) satisfy min=2 and match rows 1-2.
WITH test_cycle_reluctant_below_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_reluctant_below_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+?){2,})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Same shape with a reluctant outer bound: stops at exactly two iterations
-- even though a third A row is available.
WITH test_cycle_reluctant_outer_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_cycle_reluctant_outer_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+?){2,}?)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ------------------------------------------------------------
-- Preferment guards: the path explored second must not record
-- over a match the first one already made
-- ------------------------------------------------------------
--
-- Where two paths leave the same element the first is the preferred one, so
-- once it has recorded a match the second must not run.  Both derivations
-- cover the same rows below, so the output is the same either way; what
-- fails when a guard is removed is an assertion, in a cassert build.

-- Entering a group before skipping it.
WITH test_guard_begin_enter AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_guard_begin_enter
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)*){1,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Below the lower bound, looping back before the fast-forward.
WITH test_guard_end_below_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_guard_end_below_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){3,4})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- Between the bounds, looping before the exit.  Here the second path parks
-- a state instead of reaching FIN.
WITH test_guard_end_loop_exit AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_guard_end_loop_exit
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A (B*?)+){2,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ============================================================
-- Standard Clause 7: Formal Pattern Matching Rules
-- ISO/IEC 19075-5, Clause 7
-- ============================================================

-- ------------------------------------------------------------
-- 7.2.2 Alternation: first alternative is preferred
-- ------------------------------------------------------------

-- (A | B): A preferred over B when both could match
-- Row 1 has both A and B flags: A should be chosen (first alternative)
WITH test_alt_prefer AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['B']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_prefer
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A{1,2} | B{2,3}): all A-matches before all B-matches
-- Standard example: preferment order is AA, A, BBB, BB
-- Rows 1-2 have both A and B: greedy A{1,2} should match 1-2
WITH test_alt_quantified AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['B']),
        (4, ARRAY['B']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_quantified
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A{1,2} | B{2,3}))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A | B B | C C C): three alternatives, all viable on every row.
-- Preferment order is A, BB, CCC: the first alternative wins even though
-- the later ones would match longer.
WITH test_alt_three_way AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B','C']),
        (2, ARRAY['A','B','C']),
        (3, ARRAY['A','B','C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_three_way
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B B | C C C))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ((A | B | C)+): three alternatives ranked on every iteration, not just
-- once.  Each row carries all three flags.
WITH test_alt_three_way_loop AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B','C']),
        (2, ARRAY['A','B','C']),
        (3, ARRAY['A','B','C']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_three_way_loop
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B | C)+)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A B | A C): branches share the prefix A, so the choice is only decided
-- at row 2, which carries both B and C.  The first alternative wins there.
WITH test_alt_shared_prefix AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B','C']),
        (3, ARRAY['C']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_shared_prefix
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B | A C))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A B C | A B): the first alternative is the longer one and it fits, so
-- length and written order agree.  Compare with the reverse below.
WITH test_alt_shared_prefix_long_first AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_alt_shared_prefix_long_first
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A B C | A B))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ------------------------------------------------------------
-- 7.2.3 Concatenation: lexicographic ordering
-- ------------------------------------------------------------

-- ((A | B) (C | D)): preferment order is AC, AD, BC, BD
-- Row 1 matches A and B, Row 2 matches C and D
-- Preferred match: A then C (first alternatives in both positions)
WITH test_concat_lex AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['C','D'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_concat_lex
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A | B) (C | D))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags),
        D AS 'D' = ANY(flags)
);

-- ((A | B) C): first alt (A) fails, second alt (B) succeeds
-- Tests backtracking: row 1 has only B, row 2 has C
WITH test_concat_backtrack AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['C']),
        (3, ARRAY['A']),
        (4, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_concat_backtrack
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A | B) C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- ------------------------------------------------------------
-- 7.2.4 Quantification: greedy/reluctant, lexicographic > length
-- ------------------------------------------------------------

-- V{2,4} greedy: longer match preferred
WITH test_quant_greedy AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_greedy
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- V{2,4}? reluctant: shorter match preferred
WITH test_quant_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['A']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2,4}?)
    DEFINE
        A AS 'A' = ANY(flags)
);

-- ((A|B){1,2}) greedy: lexicographic > length
-- Standard example: preferment AA, AB, A, BA, BB, B
WITH test_quant_lex_greedy AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_lex_greedy
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B){1,2}))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ((A|B){1,2}?) reluctant: lexicographic > length
-- Standard example: preferment A, AA, AB, B, BA, BB
-- Single A preferred over any B-starting match
WITH test_quant_lex_reluctant AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_lex_reluctant
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (((A | B){1,2}?))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A+? B where every row is both A and B: stopping and continuing are both
-- viable at every step, so reluctance is actually contested.  Without the
-- overlap the reluctant path is never tested against a live alternative.
WITH test_quant_reluctant_contested AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_reluctant_contested
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A+ B over the same rows: greedy takes as many as it can while still
-- leaving a B behind.  Contrast with the reluctant case above.
WITH test_quant_greedy_contested AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A','B']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['_'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_greedy_contested
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A+ B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A+ B)+? C: the outer quantifier is reluctant, so it stops after one
-- iteration and takes the C available at row 3.  Row 3 is also an A, which
-- would let a second iteration run; reluctance declines it.
WITH test_quant_reluctant_outer AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A','C']),
        (4, ARRAY['B']),
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_reluctant_outer
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+? C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A+ B)+ C over the same rows: outer greed takes the second iteration and
-- lands on the C at row 5 instead.  Contrast with the reluctant outer above.
WITH test_quant_greedy_outer AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A','C']),
        (4, ARRAY['B']),
        (5, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_greedy_outer
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN ((A+ B)+ C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- A{2}? B: an exact bound leaves no choice, so the reluctant mark must not
-- change anything.  Overlap on rows 2-3 would expose a bound lowered to
-- {0,2}? or {1,2}?.
WITH test_quant_reluctant_exact AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_reluctant_exact
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2}? B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- A{2} B: the greedy spelling of the same bound, for contrast.
WITH test_quant_exact AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A','B']),
        (3, ARRAY['A','B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_quant_exact
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A{2} B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ------------------------------------------------------------
-- 7.2.6 Anchors (not yet implemented - syntax error expected)
-- ------------------------------------------------------------

-- ^ anchor: not yet supported
SELECT count(*) OVER w FROM (SELECT 1 AS v) t
WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (^ A) DEFINE A AS TRUE);

-- $ anchor: not yet supported
SELECT count(*) OVER w FROM (SELECT 1 AS v) t
WINDOW w AS (ORDER BY v ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    PATTERN (A $) DEFINE A AS TRUE);

-- ------------------------------------------------------------
-- 7.2.8 Infinite repetitions of empty matches
-- (Perl lower-bound stopping rule)
-- ------------------------------------------------------------
-- Standard examples from 7.2.8:
--   (A?){0,3}: allowed strings include STR00=(), STR01=(A), STR02=(empty),
--              STR03=(AA), STR04=(A,empty), STR07=(AAA), STR08=(AA,empty)
--   (A?){1,3}: same as {0,3} but STR00 excluded (min=1 not met)
--   (A?){2,3}: STR03-06 (len 2) and STR07,08,11,12 (len 3) are valid
--              STR06=(STRE,STRE) IS valid because non-final STRE at
--              position 1 fills the lower bound

-- (A??)*B: Standard 7.2.8 introductory example
-- "matched against a sequence of rows for which the only feasible
--  matching is: B"
-- A?? is reluctant, prefers empty. * is greedy but Perl rule stops
-- after empty match with min(=0) satisfied.
-- Expected: each B row matches alone (A?? empty, * stops, B matches)
WITH test_empty_reluctant_star AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_reluctant_star
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A??)* B)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A?){0,3}: min=0, nullable inner.
-- A never matches but A? matches empty, satisfying min=0 immediately.
WITH test_728_min0 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_min0
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){0,3})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- (A?){1,3}: min=1, nullable inner.
-- A never matches; one empty iteration satisfies min=1.
WITH test_728_min1 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_min1
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){1,3})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- (A?){2,3}: min=2, nullable inner.  Per ISO/IEC 19075-5 7.2.8 STR06 = (STRE STRE)
-- is valid: two empty iterations satisfy min=2.
WITH test_728_min2 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['B']),
        (3, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_min2
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- (A?){2,3} mixed: some rows match A, some don't
WITH test_728_min2_mixed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_min2_mixed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A?){2,3})
    DEFINE
        A AS 'A' = ANY(flags)
);

-- (A? | B){3}: an empty iteration below min fills the lower bound (STR06),
-- and it must outrank the later branch.  Row 2 is B only, so A? derives empty
-- there; repeating that derivation fills the remaining iterations and the
-- match ends at row 1.  Taking branch B instead would consume rows 2-3.
WITH test_728_empty_fills_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),
        (2, ARRAY['B']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_empty_fills_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B){3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- The same pattern unrolled.  Consecutive identical alternations are merged
-- into the rolled form above, so the two must agree.
WITH test_728_empty_fills_min_unrolled AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A', 'B']),
        (2, ARRAY['B']),
        (3, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_empty_fills_min_unrolled
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B) (C? | D) (E? | F))
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'A' = ANY(flags),
        D AS 'B' = ANY(flags),
        E AS 'A' = ANY(flags),
        F AS 'B' = ANY(flags)
);

-- (A? B?){2,3}: multi-element nullable body with real matches
-- Body A? B? is nullable (both optional), but A and B DO match rows.
-- Real (non-empty) iterations loop back normally; fast-forward only
-- fires as a parallel exit path (EXIT ONLY, no greedy/reluctant loop).
-- Data: alternating A, B rows (6 rows)
-- Greedy: each row gets the longest match from its starting position.
WITH test_728_multi_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['A']),
        (4, ARRAY['B']),
        (5, ARRAY['A']),
        (6, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_multi_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){2,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A? B?){2,3}: pure empty body (nothing matches A or B).
WITH test_728_multi_empty AS (
    SELECT * FROM (VALUES
        (1, ARRAY['C']),
        (2, ARRAY['C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_multi_empty
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){2,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A? B?){2,3}: mixed real and empty iterations
WITH test_728_multi_mixed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B']),
        (3, ARRAY['C']),
        (4, ARRAY['A']),
        (5, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_multi_mixed
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? B?){2,3})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);


-- The stopping rule also governs an alternation body.  On A,B the greedy
-- (A? | B)* takes A in iteration 1; in iteration 2 the preferred branch A?
-- matches empty, so the quantifier stops and B never consumes row 2.
WITH test_empty_stop_alt_body AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_empty_stop_alt_body
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- With min = max the group's own greed says nothing, so the body decides
-- whether the empty match is preferred: {n}? must equal {n}.  A? prefers to
-- consume (both forms take two rows); A?? prefers empty (both match empty).
WITH test_fixed_quant_body_greed AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A'])
    ) AS t(id, flags)
)
SELECT id, flags,
       count(*) OVER g  AS greedy_body,      -- ((A?){2})
       count(*) OVER gr AS greedy_body_rel,  -- ((A?){2}?)
       count(*) OVER r  AS rel_body,         -- ((A??){2})
       count(*) OVER rr AS rel_body_rel      -- ((A??){2}?)
FROM test_fixed_quant_body_greed
WINDOW g  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
              AFTER MATCH SKIP PAST LAST ROW PATTERN (((A?){2}))
              DEFINE A AS 'A' = ANY(flags)),
       gr AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
              AFTER MATCH SKIP PAST LAST ROW PATTERN (((A?){2}?))
              DEFINE A AS 'A' = ANY(flags)),
       r  AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
              AFTER MATCH SKIP PAST LAST ROW PATTERN (((A??){2}))
              DEFINE A AS 'A' = ANY(flags)),
       rr AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
              AFTER MATCH SKIP PAST LAST ROW PATTERN (((A??){2}?))
              DEFINE A AS 'A' = ANY(flags))
ORDER BY id;
-- (A* | B)*: A* is the preferred alternative and matches empty at row 3,
-- which ends the loop by the lower-bound stopping rule.  B is never tried,
-- so the match stops short of the B rows even though taking them would be
-- longer.  Perl agrees: (a*|b)* against "aabb" matches "aa".
WITH test_728_nullable_alt_first AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_nullable_alt_first
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A* | B)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (B | A*)*: same body, alternatives swapped.  B is preferred and consumes,
-- so no empty iteration arises and the loop reaches the B rows.
WITH test_728_nullable_alt_second AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_nullable_alt_second
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((B | A*)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A+ | B)*: the first alternative is not nullable, so it cannot produce an
-- empty iteration and the loop reaches the B rows.  Compare with (A* | B)*.
WITH test_728_nonnullable_alt_first AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_nonnullable_alt_first
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A+ | B)*)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- (A? | B){2,3}: the stop rule binds at the lower bound even when the
-- upper bound could still admit more iterations.  Iterations one and two
-- go empty via A?, which stops the loop at min; C then fails at row 1 and
-- backtracking REPLACES iteration two with B instead of extending, so the
-- match runs B, A, C = rows 1-3.  Perl agrees: (?:a?|b){2,3}c backtracks
-- the same way.  An engine that keeps iterating after the empty stop
-- returns rows 1-2 via the shorter empty-empty-B derivation instead.
WITH test_728_stop_binds_at_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A','C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_stop_binds_at_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B){2,3} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A? | B){3} C over the same rows: with an exact bound the two empty
-- iterations sit below min, so the loop must continue; the third takes B
-- and the match is rows 1-2.  Contrast with the {2,3} case above.
WITH test_728_exact_below_min AS (
    SELECT * FROM (VALUES
        (1, ARRAY['B']),
        (2, ARRAY['A','C']),
        (3, ARRAY['C'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_exact_below_min
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B){3} C)
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags),
        C AS 'C' = ANY(flags)
);

-- (A? | B){2,}: the stopping rule applies above the lower bound as well.
-- Two iterations consume the A rows and satisfy min=2; the third matches
-- empty via A? and ends the loop before any B is taken.
WITH test_728_nullable_alt_min2 AS (
    SELECT * FROM (VALUES
        (1, ARRAY['A']),
        (2, ARRAY['A']),
        (3, ARRAY['B']),
        (4, ARRAY['B'])
    ) AS t(id, flags)
)
SELECT id, flags,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_728_nullable_alt_min2
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP TO NEXT ROW
    PATTERN ((A? | B){2,})
    DEFINE
        A AS 'A' = ANY(flags),
        B AS 'B' = ANY(flags)
);

-- ------------------------------------------------------------
-- 7.3 Pattern matching in theory and practice
-- ------------------------------------------------------------

-- Standard's worked example: A? B+ with specific data
-- Preferment order: (A)(BBB), (A)(BB), (A)(B), ()(BBB), ()(BB), ()(B)
-- Row 1: A condition (price>100) is false -> A fails
-- Backtrack: empty A?, then B+ from row 1
-- Expected: rows 1-3 match as B (A? takes empty match)
WITH test_73_example AS (
    SELECT * FROM (VALUES
        (1, 60),
        (2, 70),
        (3, 40)
    ) AS t(id, price)
)
SELECT id, price,
       first_value(id) OVER w AS match_start,
       last_value(id) OVER w AS match_end
FROM test_73_example
WINDOW w AS (
    ORDER BY id
    ROWS BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING
    AFTER MATCH SKIP PAST LAST ROW
    PATTERN (A? B+)
    DEFINE
        A AS price > 100,
        B AS TRUE
);
