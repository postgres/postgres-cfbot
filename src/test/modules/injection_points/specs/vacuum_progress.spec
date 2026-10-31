# pg_stat_progress_vacuum as another session sees it while VACUUM is in its
# index-vacuum phase.  The same steps run on a heap and a FLUX table, and
# the expected values are the same for both.
#
# VACUUM waits in vac_bulkdel_one_index: its table scan has collected the
# dead TIDs and the first index is about to be vacuumed.  A cost limit of 1
# makes the scan sleep at every page, so delay_time must be positive.

setup
{
	CREATE EXTENSION injection_points;
	CREATE TABLE vp_heap (id int, v int) USING heap
		WITH (autovacuum_enabled = off, fillfactor = 10);
	CREATE TABLE vp_flux (id int, v int) USING flux
		WITH (autovacuum_enabled = off, fillfactor = 10);
	INSERT INTO vp_heap SELECT g, g FROM generate_series(1, 3000) g;
	INSERT INTO vp_flux SELECT g, g FROM generate_series(1, 3000) g;
	CREATE INDEX vp_heap_id ON vp_heap (id);
	CREATE INDEX vp_flux_id ON vp_flux (id);
	DELETE FROM vp_heap WHERE id % 3 = 0;
	DELETE FROM vp_flux WHERE id % 3 = 0;

	-- Wait until the VACUUM is at the injection point in index-vacuum
	-- round "round" (counting from 0), then show its progress.
	CREATE FUNCTION vp_progress(round int)
	RETURNS TABLE (rel regclass, phase text, total_is_size bool,
				   scanned text, vacuumed text, index_vacuum_count bigint,
				   max_dead_tuple_bytes bigint, have_dead_bytes bool,
				   num_dead_item_ids bigint, indexes_total bigint,
				   indexes_processed bigint, at_index bool, delayed bool,
				   mode text, started_by text)
	LANGUAGE plpgsql AS $$
	BEGIN
		FOR i IN 1 .. 18000 LOOP
			EXIT WHEN EXISTS (
				SELECT FROM pg_stat_progress_vacuum p
					JOIN pg_stat_activity a USING (pid)
				WHERE p.datname = current_database()
					AND p.index_vacuum_count = round
					AND a.wait_event = 'vacuum-bulkdel-one-index');
			PERFORM pg_sleep(0.01);
		END LOOP;
		RETURN QUERY
		SELECT p.relid::regclass, p.phase,
			p.heap_blks_total = pg_relation_size(p.relid) /
				current_setting('block_size')::int,
			CASE WHEN p.heap_blks_scanned = 0 THEN 'none'
				 WHEN p.heap_blks_scanned < p.heap_blks_total THEN 'some'
				 ELSE 'all' END,
			CASE WHEN p.heap_blks_vacuumed = 0 THEN 'none'
				 WHEN p.heap_blks_vacuumed < p.heap_blks_total THEN 'some'
				 ELSE 'all' END,
			p.index_vacuum_count, p.max_dead_tuple_bytes,
			p.dead_tuple_bytes > 0, p.num_dead_item_ids,
			p.indexes_total, p.indexes_processed,
			p.current_index_relid = (SELECT indexrelid FROM pg_index
									 WHERE indrelid = p.relid),
			p.delay_time > 0, p.mode, p.started_by
		FROM pg_stat_progress_vacuum p
		WHERE p.datname = current_database();
	END $$;
}

teardown
{
	DROP TABLE vp_heap;
	DROP TABLE vp_flux;
	DROP FUNCTION vp_progress(int);
	DROP EXTENSION injection_points;
}

session s1
setup
{
	SELECT injection_points_set_local();
	SELECT injection_points_attach('vacuum-bulkdel-one-index', 'wait');
	SET maintenance_work_mem = '1MB';
	-- Cost-based delay must throttle the table scan.
	SET track_cost_delay_timing = on;
	SET vacuum_cost_delay = '1ms';
	SET vacuum_cost_limit = 1;
}
step s1_vac_heap	{ VACUUM (PARALLEL 0) vp_heap; }
step s1_vac_flux	{ VACUUM (PARALLEL 0) vp_flux; }
step s1_done
{
	SELECT count(*) AS progress_rows FROM pg_stat_progress_vacuum
		WHERE pid = pg_backend_pid();
}
step s1_rows_heap	{ SELECT count(*), sum(id) FROM vp_heap; }
step s1_rows_flux	{ SELECT count(*), sum(id) FROM vp_flux; }
teardown
{
	SELECT injection_points_detach('vacuum-bulkdel-one-index');
}

session s2
step s2_round0	{ SELECT * FROM vp_progress(0); }
step s2_wake	{ SELECT injection_points_wakeup('vacuum-bulkdel-one-index'); }

permutation s1_vac_heap s2_round0 s2_wake s1_done s1_rows_heap
permutation s1_vac_flux s2_round0 s2_wake s1_done s1_rows_flux
