# Opportunistic pruning lost to a concurrent buffer pin
#
# On-access pruning gives up silently when it cannot get a cleanup lock on
# the page, i.e. when some other session holds a pin on it.  This test shows
# that prune_onaccess_missed counts exactly that, and that the very same scan
# prunes the page once the pin is gone.
#
# The page must not be prunable while the pin is being taken (otherwise the
# pinning scan prunes it itself), and must be prunable afterwards.  Hence the
# setup leaves plenty of free space on the page and "filler" fills it up.

setup
{
    CREATE TABLE prunetest (id int, v text) WITH (fillfactor = 100);
    INSERT INTO prunetest SELECT g, repeat('x', 200) FROM generate_series(1, 20) g;
    -- leaves an old pd_prune_xid behind, but not enough dead space to make a
    -- scan want to prune the page yet
    UPDATE prunetest SET v = repeat('y', 200) WHERE id <= 3;
    SELECT pg_stat_force_next_flush();
}

teardown
{
    DROP TABLE prunetest;
}

session pinner
setup		{ SET stats_fetch_consistency = 'none'; }
step p_begin	{ BEGIN; }
step p_declare	{ DECLARE c CURSOR FOR SELECT id FROM prunetest; }
# leaves the scan positioned on, and holding a pin on, the first page
step p_fetch	{ FETCH 1 FROM c; }
step p_commit	{ COMMIT; }

session filler
# fill the first page until the relation has to extend, so that a scan will
# want to prune it.  Written this way to not depend on the block size.
step f_fill
{
    DO $$
    BEGIN
        WHILE (SELECT pg_relation_size('prunetest')) <=
              current_setting('block_size')::int LOOP
            INSERT INTO prunetest
                SELECT g, repeat('z', 200) FROM generate_series(100, 110) g;
        END LOOP;
    END $$;
}

session reader
setup		{ SET stats_fetch_consistency = 'none'; }
step r_scan	{ SELECT count(*) > 0 AS scanned FROM prunetest; }
step r_ff	{ SELECT pg_stat_force_next_flush(); }
step r_stats	{ SELECT prune_onaccess, prune_onaccess_missed
		  FROM pg_stat_all_tables WHERE relname = 'prunetest'; }

# The first scan runs into the pin and gives up; the second, identical scan
# runs after the pin is released and prunes the page.
permutation p_begin p_declare p_fetch f_fill r_scan r_ff r_stats p_commit r_scan r_ff r_stats
