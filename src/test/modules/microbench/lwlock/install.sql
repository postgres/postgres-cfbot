CREATE FUNCTION bench_lwlock(
	IN n_parallel int4 DEFAULT 1,
	IN rounds int8 DEFAULT 1,
	IN iterations int8 DEFAULT 128
)
RETURNS SETOF microbench_sample
AS 'MODULE_PATHNAME', 'bench_lwlock'
LANGUAGE C;

REVOKE ALL ON FUNCTION bench_lwlock(int4, int8, int8) FROM PUBLIC;
