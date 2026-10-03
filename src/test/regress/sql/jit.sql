--
-- JIT
--
-- Exercise the LLVM JIT provider.  The default is jit = off, so the regular
-- regression suite no longer initializes the JIT.  Turn jit on and set
-- jit_above_cost to zero so that even a trivial query enables JIT and forces
-- the JIT provider to initialize.  On a working installation this simply
-- returns the result below.
--
SET jit = on;
SET jit_above_cost = 0;
SELECT 1 AS result;
RESET jit_above_cost;
RESET jit;
