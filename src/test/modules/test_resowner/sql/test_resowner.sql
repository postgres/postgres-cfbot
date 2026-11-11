CREATE EXTENSION test_resowner;

-- This is small enough that everything fits in the small array
SELECT test_resowner_priorities(2, 3);

-- Same test with more resources, to exercise the hash table
SELECT test_resowner_priorities(2, 32);

-- Basic test with lots more resources, to test extending the hash table
SELECT test_resowner_many(
  3,      -- # of different resource kinds
  100000, -- before-locks resources to remember
  500,    -- before-locks resources to forget
  100000, -- after-locks resources to remember
  500     -- after-locks resources to forget
);

-- Test resource leak warning
SELECT test_resowner_leak();

-- Negative tests, using a resource owner after release-phase has started.
set client_min_messages='warning'; -- order between ERROR and NOTICE varies
SELECT test_resowner_remember_between_phases();
SELECT test_resowner_forget_between_phases();
reset client_min_messages;

-- Reuse an auxiliary resource owner with and without a hash table.
-- For hash-table cases, force a resize before inserting into the reused hash.
-- This exposes stale entries without hanging while looking for a free slot.
SELECT test_resowner_aux(16);
SELECT test_resowner_aux(33);
SELECT test_resowner_aux(48);
SELECT test_resowner_aux(64);
