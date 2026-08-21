-- ============================================================
-- RPR NFA Tests
-- Tests for Row Pattern Recognition NFA Runtime Execution
-- ============================================================
--
-- This test suite validates the NFA (Non-deterministic Finite
-- Automaton) runtime execution engine in nodeWindowAgg.c,
-- focusing on update_reduced_frame and related functions.
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
-- min>=2 boundary and single-quantifier controls localize the behaviour: only
-- the all-reluctant case (rr) should differ.
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
-- (Same scenario as greedy-vs-reluctant comparison above; retained for
-- standalone quantifier coverage alongside A{1,3}? and A{2,3}? below)
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

-- Nested quantifier flattening must not widen the matching language (H-1).
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

-- altPriority replacement (A B C | D)
-- D branch (higher altPriority) matches first at row 1,
-- then A B C branch (lower altPriority) replaces it at row 3.
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
-- detection during DFS.  All branches must receive correct counts.
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
-- Row 1: frame [1,4], A(1-3) B(4) -> match
-- Row 2: frame [2,5], A(2-3) B(4) -> match
-- Row 3: frame [3,6], A(3) B(4) -> match
-- Row 5: frame [5,6], A(5) B(6) -> match
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
-- Single A preferred over B-starting longer match
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
-- NFA reports 3 length-0 matches (one per row); first_value / last_value
-- are NULL because the window frame for an empty match has no rows.
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
-- NFA reports 3 length-0 matches; first/last_value NULL over empty frame.
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
-- NFA reports 3 length-0 matches; first/last_value NULL over empty frame.
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
-- Rows 1-2: A matches, greedy takes 2 -> min satisfied (real match)
-- Row 3: A doesn't match, two empty iterations satisfy min=2 (length-0 match)
-- Row 4: A matches 1 real iter + 1 ff empty exit -> match 4-4
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

-- (A? B?){2,3}: multi-element nullable body with real matches
-- Body A? B? is nullable (both optional), but A and B DO match rows.
-- Real (non-empty) iterations loop back normally; fast-forward only
-- fires as a parallel exit path (EXIT ONLY, no greedy/reluctant loop).
-- Data: alternating A, B rows (6 rows)
-- Greedy: each row gets the longest match from its starting position.
-- Row 1: 3 iters (A@1,B@2)(A@3,B@4)(A@5,B@6) -> 1-6
-- Row 5: 1 real iter + 1 ff empty exit -> 5-6
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
-- NFA reports 3 length-0 matches; first/last_value NULL over empty frame.
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
-- Row 1: iter1 real (A@1,B@2), iter2 at row 3 empty -> ff exit, match 1-2
-- Row 3: C doesn't match A or B -> NULL
-- Row 4: iter1 real (A@4,B@5), iter2 at end empty -> ff exit, match 4-5
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
