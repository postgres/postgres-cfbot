--
-- FLUX: user_catalog_table is refused.
--
-- A user_catalog_table is read by logical decoding under a historic snapshot
-- that reconstructs a past catalog state from the heap's dead row versions.
-- FLUX keeps old versions in undo and reuses the tuple in place, so it cannot
-- answer those historic-snapshot reads.  Setting user_catalog_table on a FLUX
-- relation must therefore be refused, at CREATE, at ALTER TABLE ... SET, and
-- when converting a marked heap table to FLUX with SET ACCESS METHOD.  The heap
-- twin of each case must still succeed, which is the control: the refusal is
-- specific to the FLUX access method, not to the option itself.
--

-- CREATE: heap succeeds, FLUX is refused.
CREATE TABLE uct_heap_create (id int) USING heap
  WITH (user_catalog_table = true);
CREATE TABLE uct_flux_create (id int) USING flux
  WITH (user_catalog_table = true);

-- ALTER TABLE ... SET: heap succeeds, FLUX is refused.
CREATE TABLE uct_heap_set (id int) USING heap;
ALTER TABLE uct_heap_set SET (user_catalog_table = true);

CREATE TABLE uct_flux_set (id int) USING flux;
ALTER TABLE uct_flux_set SET (user_catalog_table = true);

-- SET ACCESS METHOD: converting a marked heap table to FLUX is refused with the
-- FLUX-specific message, before core's generic "cannot rewrite a catalog table"
-- restriction is reached.
CREATE TABLE uct_conv (id int) USING heap WITH (user_catalog_table = true);
ALTER TABLE uct_conv SET ACCESS METHOD flux;

-- Clearing the option, and other FLUX reloptions, still work on a FLUX table.
CREATE TABLE uct_flux_clear (id int) USING flux WITH (fillfactor = 90);
ALTER TABLE uct_flux_clear SET (user_catalog_table = false);
ALTER TABLE uct_flux_clear SET (fillfactor = 80);

DROP TABLE uct_heap_create, uct_heap_set, uct_flux_set, uct_conv, uct_flux_clear;
