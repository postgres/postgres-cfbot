--
-- FLUX overflow-pointer detection must not misalign on short-header varlenas.
--
-- FluxIsOverflowPtr() is called on every varlena column value an INSERT/UPDATE
-- forms into a FLUX tuple (flux_tuple.c), before anything is known about the
-- value.  A user varlena of 20..126 content bytes read back from a table is
-- stored with a 1-byte ("short") varlena header, so VARDATA_ANY() points one
-- byte into the datum -- an unaligned address.  Reading the 4-byte overflow
-- magic through a cast FluxOverflowPtr* there is a misaligned load: SIGBUS on
-- strict-alignment CPUs, and flagged by -fsanitize=alignment (the m32 CI
-- build).  The fix memcpy's the magic into an aligned local.
--
-- This test only needs to FORM such a tuple; it does not assert on the result,
-- it asserts (under the alignment sanitizer) that forming it reports no
-- "misaligned address".
--
CREATE TABLE flux_align_src (id int, v text, b bytea);
-- 24 bytes of content: >= sizeof(FluxOverflowPtr) (20) so the magic is read,
-- and <= 126 so it is stored with a short varlena header on the source page.
INSERT INTO flux_align_src VALUES (1, repeat('a', 24), repeat('b', 24)::bytea);

CREATE TABLE flux_align_dst (id int, v text, b bytea) USING flux;
-- SELECT deforms the short-header on-page varlenas and hands those (unaligned)
-- datums straight to the FLUX insert path, which calls FluxIsOverflowPtr on
-- each one.
INSERT INTO flux_align_dst SELECT * FROM flux_align_src;

-- An UPDATE forms new varlena values the same way.
UPDATE flux_align_dst SET v = v || 'x';

SELECT id, length(v), length(b) FROM flux_align_dst ORDER BY id;

DROP TABLE flux_align_dst;
DROP TABLE flux_align_src;
