--
-- JIT
--
-- Run a query through the LLVM JIT.  jit is off by default, so turn it on
-- and set jit_above_cost to zero, which makes even a trivial query be
-- JIT-compiled.
--
SET jit = on;
SET jit_above_cost = 0;
SELECT 1 AS result;
RESET jit_above_cost;
RESET jit;
