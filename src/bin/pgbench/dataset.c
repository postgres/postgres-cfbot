/*-------------------------------------------------------------------------
 *
 * dataset.c
 *		Shared read-only mmap dataset registry, lock-free cursors, and
 *		parallel PQputCopyData() bulk ingestion for pgbench (Pillar 1 & 5).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/dataset.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif

#include "commands.h"
#include "common/logging.h"
#include "common/pgbench_funcs.h"
#include "dataset.h"
#include "engine.h"
#include "fe_utils/string_utils.h"
#include "pqexpbuffer.h"

#define MAX_DATASETS 64
#define MAX_COPY_SPECS 64
#define COPY_CHUNK_BYTES (64 * 1024)

static PgBenchDataset datasets[MAX_DATASETS];
static int	num_datasets = 0;
static bool atexit_registered = false;

#define MAX_RESERVOIRS 64
#define MAX_WAKEUP_THREADS 1024

typedef struct CopyDatasetSpec
{
	char	   *dataset_name;
	char	   *table_spec;
} CopyDatasetSpec;

static CopyDatasetSpec copy_specs[MAX_COPY_SPECS];
int			num_copy_dataset_specs = 0;

static PgBenchReservoir reservoirs[MAX_RESERVOIRS];
static int	num_reservoirs = 0;

#define MAX_WRITER_SLOTS 1024
#define MAX_BARRIER_SLOTS 33

typedef struct PgBenchCausalClock
{
	int64		channel_watermarks[PGBENCH_MAX_WATERMARK_CHANNELS] __attribute__((aligned(64)));
	int64		global_virtual_time_us __attribute__((aligned(64)));
	int64		active_writer_ts[PGBENCH_MAX_WATERMARK_CHANNELS][MAX_WRITER_SLOTS];
	int64		max_committed_ts[PGBENCH_MAX_WATERMARK_CHANNELS];
	bool		writer_slots_initialized;
	volatile int lock;
	int			barrier_arrived[MAX_BARRIER_SLOTS];
	int			barrier_done[MAX_BARRIER_SLOTS];
	uint64		barrier_generation[MAX_BARRIER_SLOTS];
	char		pad[64];
} PgBenchCausalClock;

static PgBenchCausalClock causal_clock;
static int	thread_wakeup_fds[MAX_WAKEUP_THREADS];
static bool wakeup_fds_initialized = false;


/*
 * Cleanup all registered datasets and reservoirs at process exit.
 * Datasets remain mapped for the entire process lifetime so that borrowed
 * PGBT_VECTOR / PGBT_BYTEA pointers in client variables never dangle.
 */
void
freeAllDatasets(void)
{
	for (int i = 0; i < num_datasets; i++)
	{
		PgBenchDataset *ds = &datasets[i];

		if (ds->raw_data != NULL)
		{
#ifndef WIN32
			if (ds->is_mmapped)
				(void) munmap(unconstify(uint8 *, ds->raw_data), ds->raw_size);
			else
#endif
				pg_free(unconstify(uint8 *, ds->raw_data));
			ds->raw_data = NULL;
		}
		pg_free(ds->row_offsets);
		ds->row_offsets = NULL;
		pg_free(ds->name);
		ds->name = NULL;
		pg_free(ds->filepath);
		ds->filepath = NULL;
	}
	num_datasets = 0;

	for (int i = 0; i < num_copy_dataset_specs; i++)
	{
		pg_free(copy_specs[i].dataset_name);
		pg_free(copy_specs[i].table_spec);
	}
	num_copy_dataset_specs = 0;

	for (int i = 0; i < num_reservoirs; i++)
	{
		pg_free(reservoirs[i].name);
		pg_free(reservoirs[i].slots);
		reservoirs[i].name = NULL;
		reservoirs[i].slots = NULL;
	}
	num_reservoirs = 0;
}

/*
 * Find a loaded dataset by logical name.
 */
PgBenchDataset *
findDataset(const char *name)
{
	if (name == NULL)
		return NULL;

	for (int i = 0; i < num_datasets; i++)
	{
		if (strcmp(datasets[i].name, name) == 0)
			return &datasets[i];
	}
	return NULL;
}

/*
 * Return the row count of a dataset by name, or -1 if not found.
 */
int64
datasetCount(const char *name)
{
	PgBenchDataset *ds = findDataset(name);

	if (ds == NULL)
		return -1;
	return ds->total_rows;
}

/*
 * Build line-start offset index for CSV/TSV datasets.
 */
static void
indexDelimitedDataset(PgBenchDataset *ds)
{
	const uint8 *data = ds->raw_data;
	size_t		size = ds->raw_size;
	int64		cap = 1024;
	int64		nrows = 0;
	uint64	   *offsets;
	size_t		pos = 0;

	offsets = pg_malloc_array(uint64, cap + 1);

	while (pos < size)
	{
		size_t		line_start = pos;
		size_t		line_end;

		while (pos < size && data[pos] != '\n')
			pos++;
		line_end = pos;
		if (pos < size && data[pos] == '\n')
			pos++;

		/* Ignore empty trailing lines */
		if (line_end > line_start && data[line_end - 1] == '\r')
			line_end--;
		if (line_end == line_start)
			continue;

		if (nrows >= cap)
		{
			cap *= 2;
			offsets = pg_realloc_array(offsets, uint64, cap + 1);
		}
		offsets[nrows++] = (uint64) line_start;
	}
	offsets[nrows] = (uint64) size;

	/*
	 * If :header was explicitly specified, or if there are at least 2 rows
	 * and the first row starts with an alphabetic header identifier while
	 * the second row starts with a digit or sign, treat the first row as a
	 * header line and skip it.
	 */
	if (ds->force_header && nrows >= 1)
	{
		memmove(offsets, offsets + 1, (size_t) nrows * sizeof(uint64));
		nrows--;
	}
	else if (nrows >= 2)
	{
		uint8		c0 = data[offsets[0]];
		uint8		c1 = data[offsets[1]];

		if (((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_') &&
			((c1 >= '0' && c1 <= '9') || c1 == '-' || c1 == '+'))
		{
			memmove(offsets, offsets + 1, (size_t) nrows * sizeof(uint64));
			nrows--;
		}
	}

	ds->row_offsets = offsets;
	ds->total_rows = nrows;

	/* Count columns in the first row if non-empty */
	if (nrows > 0)
	{
		char		delim = (ds->format == DATASET_TSV) ? '\t' : ',';
		uint64		start = offsets[0];
		uint64		end = (nrows > 1) ? offsets[1] : (uint64) size;
		int			cols = 1;
		bool		in_quotes = false;

		/* Auto-detect pipe delimiter for LDBC-style CSV files without commas */
		if (ds->format == DATASET_CSV)
		{
			bool		has_comma = false;
			bool		has_pipe = false;

			for (uint64 i = start; i < end && data[i] != '\n' && data[i] != '\r'; i++)
			{
				if (data[i] == ',')
					has_comma = true;
				else if (data[i] == '|')
					has_pipe = true;
			}
			if (!has_comma && has_pipe)
				delim = '|';
		}

		for (uint64 i = start; i < end && data[i] != '\n' && data[i] != '\r'; i++)
		{
			if (data[i] == '"')
				in_quotes = !in_quotes;
			else if (!in_quotes && data[i] == (uint8) delim)
				cols++;
		}
		ds->dim = cols;
	}
	else
	{
		ds->dim = 0;
	}
	ds->record_stride = 0;
}

/*
 * Validate and compute stride/row count for binary vector/ivec files.
 */
static void
validateBinaryVectorDataset(PgBenchDataset *ds)
{
	int32		first_dim = 0;
	int32		last_dim = 0;
	size_t		elem_size;
	size_t		stride;

	if (ds->raw_size == 0)
	{
		ds->dim = 0;
		ds->record_stride = 0;
		ds->total_rows = 0;
		return;
	}

	if (ds->raw_size < sizeof(int32))
		pg_fatal("dataset \"%s\" (%s): file too small (%zu bytes)",
				 ds->name, ds->filepath, ds->raw_size);

	memcpy(&first_dim, ds->raw_data, sizeof(int32));
	if (first_dim <= 0 || first_dim > PGBENCH_MAX_VECTOR_DIM)
		pg_fatal("dataset \"%s\" (%s): invalid vector dimension %d (must be 1..%d)",
				 ds->name, ds->filepath, (int) first_dim, PGBENCH_MAX_VECTOR_DIM);

	elem_size = (ds->format == DATASET_BVECS) ? sizeof(uint8) : sizeof(float);
	stride = sizeof(int32) + (size_t) first_dim * elem_size;

	if ((ds->raw_size % stride) != 0)
		pg_fatal("dataset \"%s\" (%s): file size %zu is not a multiple of record stride %zu (dim=%d)",
				 ds->name, ds->filepath, ds->raw_size, stride, (int) first_dim);

	ds->dim = (int) first_dim;
	ds->record_stride = stride;
	ds->total_rows = (int64) (ds->raw_size / stride);

	/* Verify the final record's dimension header matches first_dim */
	if (ds->total_rows > 1)
	{
		const uint8 *last_rec = ds->raw_data + (size_t) (ds->total_rows - 1) * stride;

		memcpy(&last_dim, last_rec, sizeof(int32));
		if (last_dim != first_dim)
			pg_fatal("dataset \"%s\" (%s): inconsistent dimension in last record (%d != %d)",
					 ds->name, ds->filepath, (int) last_dim, (int) first_dim);
	}
}

/*
 * Load and register a dataset in the shared read-only dataset pool.
 */
void
registerDatasetEx(const char *name, DatasetFormat format,
				  bool force_header, const char *filepath)
{
	int			fd;
	struct stat st;
	PgBenchDataset *ds;
	uint8	   *buf = NULL;
	bool		mmapped = false;
	int			ds_idx;

	if (name == NULL || *name == '\0')
		pg_fatal("dataset name cannot be empty");
	if (filepath == NULL || *filepath == '\0')
		pg_fatal("dataset \"%s\": file path cannot be empty", name);

	if (findDataset(name) != NULL)
		pg_fatal("duplicate dataset name \"%s\"", name);

	if (num_datasets >= MAX_DATASETS)
		pg_fatal("too many datasets registered (maximum %d)", MAX_DATASETS);

	if (!atexit_registered)
	{
		atexit(freeAllDatasets);
		atexit_registered = true;
	}

	fd = open(filepath, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		pg_fatal("could not open dataset \"%s\" file \"%s\": %m", name, filepath);

	if (fstat(fd, &st) < 0)
	{
		close(fd);
		pg_fatal("could not stat dataset \"%s\" file \"%s\": %m", name, filepath);
	}

	if (st.st_size > 0)
	{
#ifndef WIN32
		/*
		 * Use read-only shared mmap on 64-bit platforms, falling back to
		 * heap allocation if mmap fails or on 32-bit platforms.
		 */
		if (sizeof(void *) >= 8)
		{
			void	   *mapped = mmap(NULL, (size_t) st.st_size, PROT_READ,
									  MAP_SHARED, fd, 0);

			if (mapped != MAP_FAILED)
			{
#ifdef MADV_WILLNEED
				(void) madvise(mapped, (size_t) st.st_size, MADV_WILLNEED);
#endif
				buf = (uint8 *) mapped;
				mmapped = true;
			}
		}
#endif
		if (!mmapped)
		{
			size_t		total_read = 0;
			size_t		to_read = (size_t) st.st_size;

			buf = (uint8 *) pg_malloc(to_read);
			while (total_read < to_read)
			{
				ssize_t		r = read(fd, buf + total_read, to_read - total_read);

				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					close(fd);
					pg_free(buf);
					pg_fatal("could not read dataset \"%s\" file \"%s\": %m",
							 name, filepath);
				}
				if (r == 0)
					break;
				total_read += (size_t) r;
			}
			if (total_read != to_read)
			{
				close(fd);
				pg_free(buf);
				pg_fatal("short read on dataset \"%s\" file \"%s\" (got %zu of %zu bytes)",
						 name, filepath, total_read, to_read);
			}
		}
	}
	close(fd);

	ds_idx = num_datasets++;
	ds = &datasets[ds_idx];
	memset(ds, 0, sizeof(PgBenchDataset));
	ds->name = pg_strdup(name);
	ds->filepath = pg_strdup(filepath);
	ds->format = format;
	ds->index = ds_idx;
	ds->force_header = force_header;
	ds->raw_data = buf;
	ds->raw_size = (size_t) st.st_size;
	ds->is_mmapped = mmapped;
	__atomic_store_n(&ds->cursor, 0, __ATOMIC_RELAXED);

	if (format == DATASET_FVECS || format == DATASET_BVECS || format == DATASET_IVECS)
		validateBinaryVectorDataset(ds);
	else
		indexDelimitedDataset(ds);

	pg_log_info("loaded dataset \"%s\" (%s): " INT64_FORMAT " rows, dim/cols=%d (%s)",
				ds->name, ds->filepath, ds->total_rows, ds->dim,
				ds->is_mmapped ? "mmap" : "heap");
}

void
registerDataset(const char *name, DatasetFormat format, const char *filepath)
{
	registerDatasetEx(name, format, false, filepath);
}

/*
 * Parse a "--dataset=<name>[:<format>][:header]=<path>" or
 * "--dataset=<name>:<format>[:header]:<path>" or
 * "--dataset=<name>=<path>" or "--dataset=<name>:<path>" specification.
 */
void
parseDatasetSpec(const char *spec)
{
	const char *sep1 = NULL;
	const char *rest;
	const char *sep2 = NULL;
	char	   *name;
	const char *filepath;
	DatasetFormat fmt = DATASET_FVECS;
	bool		explicit_fmt = false;
	bool		force_header = false;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --dataset specification: empty string");

	for (const char *p = spec; *p != '\0'; p++)
	{
		if (*p == ':' || *p == '=')
		{
			sep1 = p;
			break;
		}
	}

	if (sep1 == NULL || sep1 == spec || *(sep1 + 1) == '\0')
		pg_fatal("invalid --dataset specification \"%s\" (expected <name>[:<format>][:header]=<path> or <name>:<format>[:header]:<path>)",
				 spec);

	name = pg_malloc((sep1 - spec) + 1);
	memcpy(name, spec, sep1 - spec);
	name[sep1 - spec] = '\0';

	rest = sep1 + 1;
	for (const char *p = rest; *p != '\0'; p++)
	{
		if (*p == ':' || *p == '=')
		{
			sep2 = p;
			break;
		}
		if (*p == '/')
			break;
	}

	if (sep2 != NULL && sep2 > rest && *(sep2 + 1) != '\0')
	{
		char	   *fmt_str = pg_malloc((sep2 - rest) + 1);

		memcpy(fmt_str, rest, sep2 - rest);
		fmt_str[sep2 - rest] = '\0';

		if (pg_strcasecmp(fmt_str, "header") == 0)
		{
			force_header = true;
			explicit_fmt = false;
		}
		else if (pg_strcasecmp(fmt_str, "fvecs") == 0)
		{
			fmt = DATASET_FVECS;
			explicit_fmt = true;
		}
		else if (pg_strcasecmp(fmt_str, "bvecs") == 0)
		{
			fmt = DATASET_BVECS;
			explicit_fmt = true;
		}
		else if (pg_strcasecmp(fmt_str, "ivecs") == 0)
		{
			fmt = DATASET_IVECS;
			explicit_fmt = true;
		}
		else if (pg_strcasecmp(fmt_str, "csv") == 0 ||
				 pg_strcasecmp(fmt_str, "psv") == 0 ||
				 pg_strcasecmp(fmt_str, "pipe") == 0)
		{
			fmt = DATASET_CSV;
			explicit_fmt = true;
		}
		else if (pg_strcasecmp(fmt_str, "tsv") == 0)
		{
			fmt = DATASET_TSV;
			explicit_fmt = true;
		}
		else
			pg_fatal("unknown dataset format \"%s\" in --dataset=\"%s\" (expected fvecs, bvecs, ivecs, csv, or tsv)",
					 fmt_str, spec);

		pg_free(fmt_str);
		filepath = sep2 + 1;

		/* Check for optional ":header:" or ":header=" following format */
		{
			const char *sep3 = NULL;

			for (const char *p = filepath; *p != '\0'; p++)
			{
				if (*p == ':' || *p == '=')
				{
					sep3 = p;
					break;
				}
				if (*p == '/')
					break;
			}
			if (sep3 != NULL && (sep3 - filepath) == 6 &&
				pg_strncasecmp(filepath, "header", 6) == 0 &&
				*(sep3 + 1) != '\0')
			{
				force_header = true;
				filepath = sep3 + 1;
			}
		}
	}
	else
	{
		filepath = rest;
	}

	/*
	 * Also support trailing ":header" and/or ":<format>" suffixes after
	 * <path>, e.g. "--dataset=ldbc_p:/path/to/file.csv:pipe:header" or
	 * "--dataset=ldbc_p:/path/to/file.csv:header".
	 */
	{
		char	   *fp_copy = pg_strdup(filepath);
		int			pass;

		for (pass = 0; pass < 2; pass++)
		{
			char	   *last_colon = strrchr(fp_copy, ':');

			if (last_colon == NULL || last_colon == fp_copy ||
				strchr(last_colon, '/') != NULL)
				break;

			if (pg_strcasecmp(last_colon + 1, "header") == 0)
			{
				force_header = true;
				*last_colon = '\0';
			}
			else if (pg_strcasecmp(last_colon + 1, "csv") == 0 ||
					 pg_strcasecmp(last_colon + 1, "psv") == 0 ||
					 pg_strcasecmp(last_colon + 1, "pipe") == 0)
			{
				fmt = DATASET_CSV;
				explicit_fmt = true;
				*last_colon = '\0';
			}
			else if (pg_strcasecmp(last_colon + 1, "tsv") == 0)
			{
				fmt = DATASET_TSV;
				explicit_fmt = true;
				*last_colon = '\0';
			}
			else if (pg_strcasecmp(last_colon + 1, "fvecs") == 0)
			{
				fmt = DATASET_FVECS;
				explicit_fmt = true;
				*last_colon = '\0';
			}
			else if (pg_strcasecmp(last_colon + 1, "bvecs") == 0)
			{
				fmt = DATASET_BVECS;
				explicit_fmt = true;
				*last_colon = '\0';
			}
			else if (pg_strcasecmp(last_colon + 1, "ivecs") == 0)
			{
				fmt = DATASET_IVECS;
				explicit_fmt = true;
				*last_colon = '\0';
			}
			else
				break;
		}

		if (!explicit_fmt)
		{
			const char *ext = strrchr(fp_copy, '.');

			if (ext != NULL)
			{
				if (pg_strcasecmp(ext, ".fvecs") == 0)
					fmt = DATASET_FVECS;
				else if (pg_strcasecmp(ext, ".bvecs") == 0)
					fmt = DATASET_BVECS;
				else if (pg_strcasecmp(ext, ".ivecs") == 0)
					fmt = DATASET_IVECS;
				else if (pg_strcasecmp(ext, ".csv") == 0)
					fmt = DATASET_CSV;
				else if (pg_strcasecmp(ext, ".tsv") == 0)
					fmt = DATASET_TSV;
			}
			else if (force_header)
			{
				fmt = DATASET_CSV;
			}
		}

		registerDatasetEx(name, fmt, force_header, fp_copy);
		pg_free(fp_copy);
	}
	pg_free(name);
}


/*
 * Alias for parseDatasetSpec().
 */
void
parseVectorDatasetSpec(const char *spec)
{
	parseDatasetSpec(spec);
}

/*
 * Parse a "--vector-base=[table:][offset:]<path.fvecs>" specification.
 * Automatically registers dataset "base" (DATASET_FVECS) and registers
 * a copy-dataset target for parallel COPY into table (defaults to "vecbench_items").
 */
void
parseVectorBaseSpec(const char *spec)
{
	char	   *table = NULL;
	char	   *filepath = NULL;
	int64		offset = 1;		/* Default 1-based SQL primary key */
	const char *colon;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --vector-base specification: empty string");

	colon = strchr(spec, ':');
	if (colon != NULL)
	{
		const char *second_colon = strchr(colon + 1, ':');

		if (second_colon != NULL)
		{
			table = pnstrdup(spec, colon - spec);
			offset = strtoint64(colon + 1, false, NULL);
			filepath = pg_strdup(second_colon + 1);
		}
		else
		{
			char	   *part1 = pnstrdup(spec, colon - spec);

			if (is_an_int(part1))
			{
				table = pg_strdup("vecbench_items");
				offset = strtoint64(part1, false, NULL);
			}
			else
			{
				table = part1;
				offset = 1;
			}
			filepath = pg_strdup(colon + 1);
		}
	}
	else
	{
		table = pg_strdup("vecbench_items");
		offset = 1;
		filepath = pg_strdup(spec);
	}

	registerDatasetEx("base", DATASET_FVECS, false, filepath);
	{
		PgBenchDataset *ds = findDataset("base");

		if (ds != NULL)
			ds->id_offset = offset;
	}
	registerCopyDatasetSpec(psprintf("base:%s", table));

	pg_free(table);
	pg_free(filepath);
}

/*
 * Parse a "--ground-truth-table=[table:][offset:]<path.ivecs>" specification.
 * Automatically registers dataset "ground_truth" (DATASET_IVECS) and registers
 * a copy-dataset target for parallel COPY into table (defaults to "vecbench_ground_truth").
 */
void
parseGroundTruthTableSpec(const char *spec)
{
	char	   *table = NULL;
	char	   *filepath = NULL;
	int64		offset = 1;		/* Default 1-based SQL primary key / query ID */
	const char *colon;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --ground-truth-table specification: empty string");

	colon = strchr(spec, ':');
	if (colon != NULL)
	{
		const char *second_colon = strchr(colon + 1, ':');

		if (second_colon != NULL)
		{
			table = pnstrdup(spec, colon - spec);
			offset = strtoint64(colon + 1, false, NULL);
			filepath = pg_strdup(second_colon + 1);
		}
		else
		{
			char	   *part1 = pnstrdup(spec, colon - spec);

			if (is_an_int(part1))
			{
				table = pg_strdup("vecbench_ground_truth");
				offset = strtoint64(part1, false, NULL);
			}
			else
			{
				table = part1;
				offset = 1;
			}
			filepath = pg_strdup(colon + 1);
		}
	}
	else
	{
		table = pg_strdup("vecbench_ground_truth");
		offset = 1;
		filepath = pg_strdup(spec);
	}

	registerDatasetEx("ground_truth", DATASET_IVECS, false, filepath);
	{
		PgBenchDataset *ds = findDataset("ground_truth");

		if (ds != NULL)
			ds->id_offset = offset;
	}
	registerCopyDatasetSpec(psprintf("ground_truth:%s", table));

	pg_free(table);
	pg_free(filepath);
}

/*
 * Register a "--copy-dataset=<dataset>:<table>[(cols)]" specification.
 */
void
registerCopyDatasetSpec(const char *spec)
{
	const char *colon;
	char	   *ds_name;
	const char *table_spec;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --copy-dataset specification: empty string");

	colon = strchr(spec, ':');
	if (colon == NULL || colon == spec || *(colon + 1) == '\0')
		pg_fatal("invalid --copy-dataset specification \"%s\" (expected <dataset>:<table>[(cols)])",
				 spec);

	if (num_copy_dataset_specs >= MAX_COPY_SPECS)
		pg_fatal("too many --copy-dataset specifications (maximum %d)", MAX_COPY_SPECS);

	ds_name = pg_malloc((colon - spec) + 1);
	memcpy(ds_name, spec, colon - spec);
	ds_name[colon - spec] = '\0';
	table_spec = colon + 1;

	copy_specs[num_copy_dataset_specs].dataset_name = ds_name;
	copy_specs[num_copy_dataset_specs].table_spec = pg_strdup(table_spec);
	num_copy_dataset_specs++;
}

/*
 * Parse a scalar token from a CSV/TSV cell into a PgBenchValue.
 */
static void
parseDelimitedCellValue(const char *token, bool was_quoted, PgBenchValue *val)
{
	size_t		len = strlen(token);
	int64		ival;
	double		dval;

	if (!was_quoted && (len == 0 || pg_strcasecmp(token, "NULL") == 0 ||
						strcmp(token, "\\N") == 0))
	{
		setNullValue(val);
		return;
	}

	if (len >= 3 && token[0] == '[' && token[len - 1] == ']' &&
		parseVectorLiteral(token, val))
		return;

	if (len >= 2 && token[0] == '{' && token[len - 1] == '}' &&
		parseArrayLiteral(token, val))
		return;

	if (len >= 2 && token[0] == '\\' && (token[1] == 'x' || token[1] == 'X') &&
		parseByteaLiteral(token, val))
		return;

	if (!was_quoted)
	{
		if (pg_strcasecmp(token, "true") == 0)
		{
			setBoolValue(val, true);
			return;
		}
		if (pg_strcasecmp(token, "false") == 0)
		{
			setBoolValue(val, false);
			return;
		}
		if (is_an_int(token) && strtoint64(token, true, &ival))
		{
			setIntValue(val, ival);
			return;
		}
		if (strtodouble(token, true, &dval))
		{
			setDoubleValue(val, dval);
			return;
		}
	}

	setTextValue(val, token);
}

/*
 * Parse a row from a CSV/TSV dataset into an array of column values, or extract
 * a single column when target_col >= 0.
 */
static bool
parseDelimitedRow(PgBenchDataset *ds, int64 row_idx, int target_col,
				  PgBenchValue *retval)
{
	uint64		start = ds->row_offsets[row_idx];
	uint64		end = ds->row_offsets[row_idx + 1];
	const char *p = (const char *) (ds->raw_data + start);
	const char *line_end = (const char *) (ds->raw_data + end);
	char		delim = (ds->format == DATASET_TSV) ? '\t' : ',';
	PgBenchValue *cols = NULL;
	int			ncols = 0;
	int			cap = 0;

	/* Strip trailing CR/LF */
	while (line_end > p && (line_end[-1] == '\n' || line_end[-1] == '\r'))
		line_end--;

	/* Auto-detect pipe delimiter if CSV line has '|' and no ',' */
	if (ds->format == DATASET_CSV)
	{
		bool		has_comma = false;
		bool		has_pipe = false;

		for (const char *scan = p; scan < line_end; scan++)
		{
			if (*scan == ',')
				has_comma = true;
			else if (*scan == '|')
				has_pipe = true;
		}
		if (!has_comma && has_pipe)
			delim = '|';
	}

	while (p <= line_end)
	{
		PQExpBufferData cell;
		bool		was_quoted = false;
		PgBenchValue cell_val;

		initPQExpBuffer(&cell);

		if (p < line_end && *p == '"')
		{
			was_quoted = true;
			p++;
			while (p < line_end)
			{
				if (*p == '"')
				{
					if ((p + 1) < line_end && p[1] == '"')
					{
						appendPQExpBufferChar(&cell, '"');
						p += 2;
					}
					else
					{
						p++;
						break;
					}
				}
				else
				{
					appendPQExpBufferChar(&cell, *p++);
				}
			}
			while (p < line_end && *p != delim)
				p++;
		}
		else
		{
			const char *tok_start = p;

			while (p < line_end && *p != delim)
				p++;
			appendBinaryPQExpBuffer(&cell, tok_start, p - tok_start);
		}

		if (target_col >= 0)
		{
			if (ncols == target_col)
			{
				parseDelimitedCellValue(cell.data, was_quoted, retval);
				termPQExpBuffer(&cell);
				return true;
			}
			termPQExpBuffer(&cell);
			ncols++;
		}
		else
		{
			parseDelimitedCellValue(cell.data, was_quoted, &cell_val);
			termPQExpBuffer(&cell);

			if (ncols >= cap)
			{
				cap = (cap == 0) ? 8 : cap * 2;
				cols = pg_realloc_array(cols, PgBenchValue, cap);
			}
			cols[ncols++] = cell_val;
		}

		if (p < line_end && *p == delim)
		{
			p++;
			/* Handle trailing empty cell after final delimiter */
			if (p == line_end)
			{
				if (target_col >= 0)
				{
					if (ncols == target_col)
					{
						setNullValue(retval);
						return true;
					}
					ncols++;
				}
				else
				{
					if (ncols >= cap)
					{
						cap = (cap == 0) ? 8 : cap * 2;
						cols = pg_realloc_array(cols, PgBenchValue, cap);
					}
					setNullValue(&cols[ncols++]);
				}
				break;
			}
		}
		else
			break;
	}

	if (target_col >= 0)
	{
		pg_log_error("dataset \"%s\": column index %d out of range (row " INT64_FORMAT " has %d columns)",
					 ds->name, target_col, row_idx, ncols);
		return false;
	}

	if (ncols == 1)
	{
		*retval = cols[0];
		pg_free(cols);
	}
	else
	{
		retval->type = PGBT_ARRAY;
		retval->u.aval.nelems = ncols;
		retval->u.aval.elems = cols;
	}
	return true;
}

/*
 * Retrieve a row or cell from a dataset in O(1) time.
 * When col_idx < 0:
 *   - DATASET_FVECS: returns zero-copy borrowed PGBT_VECTOR
 *   - DATASET_BVECS: returns heap-allocated PGBT_VECTOR
 *   - DATASET_IVECS: returns PGBT_ARRAY of PGBT_INT
 *   - DATASET_CSV / DATASET_TSV: returns scalar if 1 column, else PGBT_ARRAY of columns
 */
bool
datasetGetRow(PgBenchDataset *ds, int64 row_idx, int col_idx,
			  PgBenchValue *retval)
{
	if (ds == NULL)
		return false;

	if (row_idx < 0 || row_idx >= ds->total_rows)
	{
		pg_log_error("dataset \"%s\": row index " INT64_FORMAT " out of bounds (total_rows=" INT64_FORMAT ")",
					 ds->name, row_idx, ds->total_rows);
		return false;
	}

	switch (ds->format)
	{
		case DATASET_FVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const float *floats = (const float *) (rec + sizeof(int32));

				setVectorValue(retval, floats, ds->dim, true);
				return true;
			}

		case DATASET_BVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const uint8 *bytes = rec + sizeof(int32);
				float	   *floats = pg_malloc_array(float, ds->dim);

				for (int i = 0; i < ds->dim; i++)
					floats[i] = (float) bytes[i];

				retval->type = PGBT_VECTOR;
				retval->u.vval.dim = ds->dim;
				retval->u.vval.is_borrowed = false;
				retval->u.vval.elems = floats;
				return true;
			}

		case DATASET_IVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const int32 *ints = (const int32 *) (rec + sizeof(int32));
				PgBenchValue *elems = pg_malloc_array(PgBenchValue, ds->dim);
				int64		offset = (col_idx > 0) ? (int64) col_idx : ds->id_offset;

				for (int i = 0; i < ds->dim; i++)
				{
					int32		v;

					memcpy(&v, &ints[i], sizeof(int32));
					setIntValue(&elems[i], (int64) v + offset);
				}

				retval->type = PGBT_ARRAY;
				retval->u.aval.nelems = ds->dim;
				retval->u.aval.elems = elems;
				return true;
			}

		case DATASET_CSV:
		case DATASET_TSV:
			return parseDelimitedRow(ds, row_idx, col_idx, retval);

		default:
			Assert(false);
			return false;
	}
}

/*
 * Advance the dataset's cursor according to mode and return the next row index
 * (or PGBT_NULL when CURSOR_ONCE_GLOBAL or CURSOR_PARTITIONED is exhausted).
 */
bool
datasetNextId(PgBenchDataset *ds, CState *st, DatasetCursorMode mode,
			  PgBenchValue *retval)
{
	uint64		idx;

	if (ds == NULL)
		return false;

	if (ds->total_rows <= 0)
	{
		setNullValue(retval);
		return true;
	}

	switch (mode)
	{
		case CURSOR_ONCE_GLOBAL:
			idx = __atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED);
			if (idx >= (uint64) ds->total_rows)
			{
				setNullValue(retval);
				return true;
			}
			setIntValue(retval, (int64) idx);
			return true;

		case CURSOR_PARTITIONED:
			{
				int			num_streams = 1;
				int			stream_id = 0;
				uint64		k = 0;
				uint64		row_idx;

				if (st != NULL)
				{
					num_streams = (st->group_id >= 0 && st->group_id < num_groups) ?
						client_groups[st->group_id].nclients : nclients;
					if (num_streams < 1)
						num_streams = 1;
					stream_id = (st->group_id >= 0) ? st->stream_id : st->id;
					if (stream_id < 0)
						stream_id = 0;
					if (ds->index >= 0 && ds->index < MAX_DATASETS)
						k = st->dataset_cursors[ds->index]++;
					else
						k = __atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED);
				}
				else
				{
					k = __atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED);
				}

				row_idx = k * (uint64) num_streams + (uint64) stream_id;
				if (row_idx >= (uint64) ds->total_rows)
				{
					setNullValue(retval);
					return true;
				}
				setIntValue(retval, (int64) row_idx);
				return true;
			}

		case CURSOR_SAMPLE:
			{
				int64		sample_idx;

				if (st != NULL)
					sample_idx = pgbench_random(&st->cs_func_rs, 0, ds->total_rows - 1);
				else
					sample_idx = (int64) (__atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED) %
										  (uint64) ds->total_rows);
				setIntValue(retval, sample_idx);
				return true;
			}

		case CURSOR_WRAP_ROUND_ROBIN:
		default:
			idx = __atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED);
			setIntValue(retval, (int64) (idx % (uint64) ds->total_rows));
			return true;
	}
}

/*
 * Strip optional leading ':' from a variable name token in meta-commands.
 */
static const char *
normalizeVarArg(const char *arg)
{
	if (arg != NULL && arg[0] == ':')
		return arg + 1;
	return arg;
}

/*
 * Handler for \dataset_bind <dataset> <cursor_mode> <var1> [<var2> ...]
 */
ConnectionStateEnum
handle_cmd_dataset_bind(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *ds_name = cmd->argv[1];
	const char *mode_str = cmd->argv[2];
	PgBenchDataset *ds;
	DatasetCursorMode mode;
	PgBenchValue idx_val;
	int64		row_idx;
	int			var_start = 3;
	int			nvars = cmd->argc - 3;

	ds = findDataset(ds_name);
	if (ds == NULL)
	{
		pg_log_error("client %d: unknown dataset \"%s\" in \\dataset_bind",
					 st->id, ds_name);
		return CSTATE_ABORTED;
	}

	if (pg_strcasecmp(mode_str, "at") == 0)
	{
		var_start = 4;
		nvars = cmd->argc - 4;
		if (cmd->expr != NULL)
		{
			PgBenchValue ev;
			int64		rval = 0;

			if (!evaluateExpr(st, cmd->expr, &ev) || !coerceToInt(&ev, &rval))
			{
				freeValue(&ev);
				pg_log_error("client %d: invalid row index expression in \\dataset_bind",
							 st->id);
				return CSTATE_ABORTED;
			}
			freeValue(&ev);
			setIntValue(&idx_val, rval);
		}
		else
		{
			mode_str = cmd->argv[3];
			if (mode_str[0] == ':')
			{
				Variable   *var = lookupVariable(&st->variables, (char *) (mode_str + 1));
				int64		rval;

				if (var == NULL || !coerceToInt(&var->value, &rval))
				{
					pg_log_error("client %d: invalid row index variable \"%s\" in \\dataset_bind",
								 st->id, mode_str);
					return CSTATE_ABORTED;
				}
				setIntValue(&idx_val, rval);
			}
			else if (is_an_int(mode_str))
			{
				int64		rval = 0;

				if (!strtoint64(mode_str, true, &rval))
					return CSTATE_ABORTED;
				setIntValue(&idx_val, rval);
			}
			else
			{
				pg_log_error("client %d: invalid row index \"%s\" after AT in \\dataset_bind",
							 st->id, mode_str);
				return CSTATE_ABORTED;
			}
		}
	}
	else if (pg_strcasecmp(mode_str, "wrap") == 0 ||
			 pg_strcasecmp(mode_str, "next") == 0 ||
			 pg_strcasecmp(mode_str, "round_robin") == 0 ||
			 pg_strcasecmp(mode_str, "wrap_round_robin") == 0)
	{
		mode = CURSOR_WRAP_ROUND_ROBIN;
		if (!datasetNextId(ds, st, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (pg_strcasecmp(mode_str, "once") == 0 ||
			 pg_strcasecmp(mode_str, "once_global") == 0)
	{
		mode = CURSOR_ONCE_GLOBAL;
		if (!datasetNextId(ds, st, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (pg_strcasecmp(mode_str, "partitioned") == 0)
	{
		mode = CURSOR_PARTITIONED;
		if (!datasetNextId(ds, st, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (pg_strcasecmp(mode_str, "sample") == 0 ||
			 pg_strcasecmp(mode_str, "random") == 0)
	{
		mode = CURSOR_SAMPLE;
		if (!datasetNextId(ds, st, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (mode_str[0] == ':')
	{
		Variable   *var = lookupVariable(&st->variables, (char *) (mode_str + 1));
		int64		rval;

		if (var == NULL || !coerceToInt(&var->value, &rval))
		{
			pg_log_error("client %d: invalid row index variable \"%s\" in \\dataset_bind",
						 st->id, mode_str);
			return CSTATE_ABORTED;
		}
		setIntValue(&idx_val, rval);
	}
	else if (is_an_int(mode_str))
	{
		int64		rval = 0;

		if (!strtoint64(mode_str, true, &rval))
			return CSTATE_ABORTED;
		setIntValue(&idx_val, rval);
	}
	else
	{
		pg_log_error("client %d: invalid cursor mode \"%s\" in \\dataset_bind (expected wrap, once, partitioned, sample, or row index)",
					 st->id, mode_str);
		return CSTATE_ABORTED;
	}


	/* Exhausted ONCE_GLOBAL cursor: set all target variables to NULL */
	if (idx_val.type == PGBT_NULL)
	{
		PgBenchValue null_v;

		setNullValue(&null_v);
		for (int i = 0; i < nvars; i++)
		{
			const char *vname = normalizeVarArg(cmd->argv[var_start + i]);

			if (!putVariableValue(&st->variables, "dataset_bind", (char *) vname, &null_v))
				return CSTATE_ABORTED;
		}
		return CSTATE_END_COMMAND;
	}

	row_idx = idx_val.u.ival;

	if (ds->format == DATASET_FVECS || ds->format == DATASET_BVECS ||
		ds->format == DATASET_IVECS)
	{
		PgBenchValue row_val;

		if (!datasetGetRow(ds, row_idx, -1, &row_val))
			return CSTATE_ABORTED;

		if (nvars == 1)
		{
			const char *vname = normalizeVarArg(cmd->argv[var_start]);
			bool		ok = putVariableValue(&st->variables, "dataset_bind",
											  (char *) vname, &row_val);

			freeValue(&row_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
		else
		{
			const char *id_var = normalizeVarArg(cmd->argv[var_start]);
			const char *vec_var = normalizeVarArg(cmd->argv[var_start + 1]);
			bool		ok;

			if (!putVariableInt(&st->variables, "dataset_bind", (char *) id_var, row_idx))
			{
				freeValue(&row_val);
				return CSTATE_ABORTED;
			}
			ok = putVariableValue(&st->variables, "dataset_bind",
								  (char *) vec_var, &row_val);
			freeValue(&row_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
	}
	else
	{
		for (int i = 0; i < nvars; i++)
		{
			const char *vname = normalizeVarArg(cmd->argv[var_start + i]);
			PgBenchValue col_val;
			bool		ok;

			if (!datasetGetRow(ds, row_idx, i, &col_val))
				return CSTATE_ABORTED;
			if (strcmp(vname, "_r_sql") == 0 && col_val.type == PGBT_TEXT)
				ok = putVariable(&st->variables, "dataset_bind",
								 (char *) vname,
								 col_val.u.sval != NULL ? col_val.u.sval : "");
			else
				ok = putVariableValue(&st->variables, "dataset_bind",
									  (char *) vname, &col_val);
			freeValue(&col_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
	}

	return CSTATE_END_COMMAND;
}

/*
 * Determine the number of columns targeted by table_spec for vector COPY.
 * If table_spec contains "(col1, col2, ...)", count commas + 1.
 * Otherwise, probe the table with SELECT * FROM <table> LIMIT 0.
 */
static int
detectTargetColumnCount(PGconn *con, const char *table_spec)
{
	const char *lparen = strchr(table_spec, '(');

	if (lparen != NULL)
	{
		int			cols = 1;

		for (const char *p = lparen + 1; *p != '\0' && *p != ')'; p++)
		{
			if (*p == ',')
				cols++;
		}
		return cols;
	}
	else
	{
		PQExpBufferData q;
		PGresult   *res;
		int			ncols = 2;

		initPQExpBuffer(&q);
		appendPQExpBuffer(&q, "SELECT * FROM %s LIMIT 0", table_spec);
		res = PQexec(con, q.data);
		termPQExpBuffer(&q);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
			ncols = PQnfields(res);
		PQclear(res);
		return ncols;
	}
}

/*
 * Stream the slice [start_row, end_row) of a dataset into PostgreSQL via
 * PQputCopyData() + PQputCopyEnd().
 */
bool
copyDatasetSlice(PGconn *con, PgBenchDataset *ds, const char *table_spec,
				 int client_id, int num_clients)
{
	int64		start_row;
	int64		end_row;
	PQExpBufferData copy_sql;
	PGresult   *res;

	if (ds == NULL || con == NULL)
		return false;

	if (num_clients <= 0)
		num_clients = 1;
	if (client_id < 0)
		client_id = 0;

	start_row = (ds->total_rows * (int64) client_id) / (int64) num_clients;
	end_row = (ds->total_rows * (int64) (client_id + 1)) / (int64) num_clients;

	if (start_row >= end_row)
		return true;

	initPQExpBuffer(&copy_sql);

	if (ds->format == DATASET_CSV)
	{
		char		delim = ',';
		uint64		s0 = ds->row_offsets[0];
		uint64		e0 = ds->row_offsets[1];
		bool		has_comma = false;
		bool		has_pipe = false;

		for (uint64 i = s0; i < e0 && ds->raw_data[i] != '\n'; i++)
		{
			if (ds->raw_data[i] == ',')
				has_comma = true;
			else if (ds->raw_data[i] == '|')
				has_pipe = true;
		}
		if (!has_comma && has_pipe)
			delim = '|';

		appendPQExpBuffer(&copy_sql,
						  "COPY %s FROM STDIN WITH (FORMAT csv, DELIMITER '%c')",
						  table_spec, delim);
	}
	else
	{
		appendPQExpBuffer(&copy_sql, "COPY %s FROM STDIN", table_spec);
	}

	if (ds->format == DATASET_FVECS || ds->format == DATASET_BVECS ||
		ds->format == DATASET_IVECS)
	{
		int			target_cols = detectTargetColumnCount(con, table_spec);
		PQExpBufferData chunk;

		res = PQexec(con, copy_sql.data);
		termPQExpBuffer(&copy_sql);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			pg_log_error("COPY failed for dataset \"%s\": %s",
						 ds->name, PQerrorMessage(con));
			PQclear(res);
			return false;
		}
		PQclear(res);

		initPQExpBuffer(&chunk);
		for (int64 r = start_row; r < end_row; r++)
		{
			PgBenchValue v;
			char	   *formatted;

			if (!datasetGetRow(ds, r, -1, &v))
			{
				termPQExpBuffer(&chunk);
				(void) PQputCopyEnd(con, "dataset read error");
				res = PQgetResult(con);
				PQclear(res);
				return false;
			}
			formatted = formatValue(&v);
			freeValue(&v);

			if (target_cols >= 2)
			{
				appendPQExpBuffer(&chunk, INT64_FORMAT "\t%s", r + ds->id_offset, formatted);
				for (int c = 2; c < target_cols; c++)
					appendPQExpBufferStr(&chunk, "\t\\N");
				appendPQExpBufferChar(&chunk, '\n');
			}
			else
			{
				appendPQExpBuffer(&chunk, "%s\n", formatted);
			}
			pg_free(formatted);

			if (chunk.len >= COPY_CHUNK_BYTES)
			{
				if (PQputCopyData(con, chunk.data, (int) chunk.len) != 1)
				{
					pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
								 ds->name, PQerrorMessage(con));
					termPQExpBuffer(&chunk);
					return false;
				}
				resetPQExpBuffer(&chunk);
			}
		}

		if (chunk.len > 0)
		{
			if (PQputCopyData(con, chunk.data, (int) chunk.len) != 1)
			{
				pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
							 ds->name, PQerrorMessage(con));
				termPQExpBuffer(&chunk);
				return false;
			}
		}
		termPQExpBuffer(&chunk);
	}
	else
	{
		uint64		start_byte = ds->row_offsets[start_row];
		uint64		end_byte = ds->row_offsets[end_row];
		uint64		offset = start_byte;

		res = PQexec(con, copy_sql.data);
		termPQExpBuffer(&copy_sql);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			pg_log_error("COPY failed for dataset \"%s\": %s",
						 ds->name, PQerrorMessage(con));
			PQclear(res);
			return false;
		}
		PQclear(res);

		while (offset < end_byte)
		{
			uint64		rem = end_byte - offset;
			int			chunk_len = (rem > COPY_CHUNK_BYTES) ? COPY_CHUNK_BYTES : (int) rem;

			if (PQputCopyData(con, (const char *) (ds->raw_data + offset), chunk_len) != 1)
			{
				pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
							 ds->name, PQerrorMessage(con));
				return false;
			}
			offset += (uint64) chunk_len;
		}
	}

	if (PQputCopyEnd(con, NULL) != 1)
	{
		pg_log_error("PQputCopyEnd failed for dataset \"%s\": %s",
					 ds->name, PQerrorMessage(con));
		return false;
	}

	res = PQgetResult(con);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("COPY end failed for dataset \"%s\": %s",
					 ds->name, PQerrorMessage(con));
		PQclear(res);
		return false;
	}
	PQclear(res);

	/* Drain any remaining NULL result from PQgetResult */
	while ((res = PQgetResult(con)) != NULL)
		PQclear(res);

	return true;
}

typedef struct CopyDatasetWorker
{
	THREAD_T	thread;
	PGconn	   *con;
	PgBenchDataset *ds;
	const char *table_spec;
	int			worker_id;
	int			num_workers;
	bool		ok;
} CopyDatasetWorker;

static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC
copyDatasetWorkerRun(void *arg)
{
	CopyDatasetWorker *w = (CopyDatasetWorker *) arg;

	w->ok = copyDatasetSlice(w->con, w->ds, w->table_spec,
							 w->worker_id, w->num_workers);
	THREAD_FUNC_RETURN;
}

/*
 * Execute all registered --copy-dataset specs using up to nthreads parallel workers.
 */
void
runRegisteredCopyDatasets(PGconn *con)
{
	for (int i = 0; i < num_copy_dataset_specs; i++)
	{
		PgBenchDataset *ds = findDataset(copy_specs[i].dataset_name);
		int			num_workers = (nthreads > 1) ? nthreads : 1;

		if (ds == NULL)
			pg_fatal("--copy-dataset references unknown dataset \"%s\"",
					 copy_specs[i].dataset_name);

		if (ds->total_rows > 0 && (int64) num_workers > ds->total_rows)
			num_workers = (int) ds->total_rows;

		pg_log_info("copying dataset \"%s\" (" INT64_FORMAT " rows) into %s (%d workers)...",
					ds->name, ds->total_rows, copy_specs[i].table_spec, num_workers);

		if (num_workers <= 1)
		{
			if (!copyDatasetSlice(con, ds, copy_specs[i].table_spec, 0, 1))
				pg_fatal("failed to copy dataset \"%s\" into %s",
						 ds->name, copy_specs[i].table_spec);
		}
		else
		{
			CopyDatasetWorker *workers = pg_malloc0_array(CopyDatasetWorker, num_workers);
			bool		all_ok = true;

			workers[0].con = con;
			workers[0].ds = ds;
			workers[0].table_spec = copy_specs[i].table_spec;
			workers[0].worker_id = 0;
			workers[0].num_workers = num_workers;

			for (int w = 1; w < num_workers; w++)
			{
				workers[w].con = doConnect();
				if (workers[w].con == NULL)
					pg_fatal("could not create connection for --copy-dataset worker %d", w);
				workers[w].ds = ds;
				workers[w].table_spec = copy_specs[i].table_spec;
				workers[w].worker_id = w;
				workers[w].num_workers = num_workers;

				errno = THREAD_CREATE(&workers[w].thread, copyDatasetWorkerRun, &workers[w]);
				if (errno != 0)
					pg_fatal("could not create --copy-dataset worker thread: %m");
			}

			workers[0].ok = copyDatasetSlice(workers[0].con, ds,
											 copy_specs[i].table_spec,
											 0, num_workers);
			if (!workers[0].ok)
				all_ok = false;

			for (int w = 1; w < num_workers; w++)
			{
				THREAD_JOIN(workers[w].thread);
				PQfinish(workers[w].con);
				if (!workers[w].ok)
					all_ok = false;
			}
			pg_free(workers);

			if (!all_ok)
				pg_fatal("failed to copy dataset \"%s\" into %s",
						 ds->name, copy_specs[i].table_spec);
		}
	}
}

/*
 * Handler for \copy_dataset <dataset> <table> [(cols)]
 */
ConnectionStateEnum
handle_cmd_copy_dataset(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *ds_name = cmd->argv[1];
	PQExpBufferData table_spec;
	PgBenchDataset *ds;
	bool		ok;

	ds = findDataset(ds_name);
	if (ds == NULL)
	{
		pg_log_error("client %d: unknown dataset \"%s\" in \\copy_dataset",
					 st->id, ds_name);
		return CSTATE_ABORTED;
	}

	initPQExpBuffer(&table_spec);
	for (int i = 2; i < cmd->argc; i++)
	{
		if (i > 2)
			appendPQExpBufferChar(&table_spec, ' ');
		appendPQExpBufferStr(&table_spec, cmd->argv[i]);
	}

	ok = copyDatasetSlice(st->con, ds, table_spec.data, st->id, nclients);
	termPQExpBuffer(&table_spec);

	if (!ok)
	{
		commandFailed(st, "copy_dataset", "bulk dataset COPY failed");
		return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}

/*-------------------------------------------------------------------------
 * Pillar 2: Global Monotonic Clock & Causal Watermark Coordination
 *-------------------------------------------------------------------------
 */

void
registerThreadWakeupPipe(int tid, int write_fd)
{
	if (!wakeup_fds_initialized)
	{
		for (int i = 0; i < MAX_WAKEUP_THREADS; i++)
			thread_wakeup_fds[i] = -1;
		wakeup_fds_initialized = true;
	}
	if (tid >= 0 && tid < MAX_WAKEUP_THREADS)
		__atomic_store_n(&thread_wakeup_fds[tid], write_fd, __ATOMIC_RELEASE);
}

void
unregisterThreadWakeupPipe(int tid)
{
	if (tid >= 0 && tid < MAX_WAKEUP_THREADS)
		__atomic_store_n(&thread_wakeup_fds[tid], -1, __ATOMIC_RELEASE);
}

void
wakeWatermarkWaiters(void)
{
	if (!wakeup_fds_initialized)
		return;

	for (int i = 0; i < nthreads && i < MAX_WAKEUP_THREADS; i++)
	{
		int			wfd = __atomic_load_n(&thread_wakeup_fds[i], __ATOMIC_ACQUIRE);

		if (wfd >= 0)
		{
			char		byte = 'W';
			ssize_t		rc;

			rc = write(wfd, &byte, 1);
			(void) rc;
		}
	}
}

static inline void
lockCausalClock(void)
{
	while (__atomic_test_and_set(&causal_clock.lock, __ATOMIC_ACQUIRE))
	{
		/* spin */
	}
	if (!causal_clock.writer_slots_initialized)
	{
		for (int c = 0; c < PGBENCH_MAX_WATERMARK_CHANNELS; c++)
		{
			for (int s = 0; s < MAX_WRITER_SLOTS; s++)
				causal_clock.active_writer_ts[c][s] = PG_INT64_MAX;
			causal_clock.max_committed_ts[c] = 0;
		}
		causal_clock.writer_slots_initialized = true;
	}
}

static inline void
unlockCausalClock(void)
{
	__atomic_clear(&causal_clock.lock, __ATOMIC_RELEASE);
}

/*
 * Recompute safe lower-bound watermark for channel chan while holding
 * causal_clock.lock.
 *
 * safe_watermark = min(max_committed_ts[chan], min_i(active_writer_ts[chan][i]) - 1)
 */
static int64
recomputeSafeWatermarkLocked(int chan, bool *advanced_out)
{
	int64		min_active = PG_INT64_MAX;
	int64		max_comm = causal_clock.max_committed_ts[chan];
	int64		safe_wm;
	int64		cur_wm;

	for (int s = 0; s < MAX_WRITER_SLOTS; s++)
	{
		int64		ats = causal_clock.active_writer_ts[chan][s];

		if (ats < min_active)
			min_active = ats;
	}

	if (min_active < PG_INT64_MAX)
	{
		int64		gate = (min_active > PG_INT64_MIN) ? (min_active - 1) : PG_INT64_MIN;

		safe_wm = (max_comm > gate) ? gate : max_comm;
	}
	else
	{
		safe_wm = max_comm;
	}

	cur_wm = __atomic_load_n(&causal_clock.channel_watermarks[chan], __ATOMIC_RELAXED);
	if (safe_wm > cur_wm)
	{
		__atomic_store_n(&causal_clock.channel_watermarks[chan], safe_wm, __ATOMIC_RELEASE);
		if (advanced_out != NULL)
			*advanced_out = true;
		return safe_wm;
	}
	if (advanced_out != NULL)
		*advanced_out = false;
	return cur_wm;
}

int64
getWatermarkChan(int chan)
{
	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;
	return __atomic_load_n(&causal_clock.channel_watermarks[chan], __ATOMIC_ACQUIRE);
}

int64
getWatermark(void)
{
	return getWatermarkChan(0);
}

int64
setWatermarkChan(int chan, int64 wm)
{
	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;
	lockCausalClock();
	causal_clock.max_committed_ts[chan] = wm;
	__atomic_store_n(&causal_clock.channel_watermarks[chan], wm, __ATOMIC_RELEASE);
	unlockCausalClock();
	wakeWatermarkWaiters();
	return wm;
}

int64
setWatermark(int64 wm)
{
	return setWatermarkChan(0, wm);
}

int64
advanceWatermarkChan(int chan, int64 wm)
{
	int64		res;
	bool		advanced = false;

	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;

	lockCausalClock();
	if (wm > causal_clock.max_committed_ts[chan])
		causal_clock.max_committed_ts[chan] = wm;
	res = recomputeSafeWatermarkLocked(chan, &advanced);
	unlockCausalClock();

	if (advanced)
		wakeWatermarkWaiters();
	return res;
}

int64
advanceWatermark(int64 wm)
{
	return advanceWatermarkChan(0, wm);
}

/*
 * Register an in-flight dependent write timestamp for client st on channel chan.
 */
int64
beginDependentWriteChan(CState *st, int chan, int64 event_ts)
{
	int			slot = (st != NULL && st->id >= 0) ? (st->id % MAX_WRITER_SLOTS) : 0;
	int64		cur_wm;

	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;

	if (st != NULL && st->has_active_dep_write && st->active_dep_write_chan != chan)
		cancelDependentWrite(st);

	lockCausalClock();
	if (causal_clock.active_writer_ts[chan][slot] != PG_INT64_MAX &&
		causal_clock.active_writer_ts[chan][slot] != event_ts)
	{
		for (int s = 0; s < MAX_WRITER_SLOTS; s++)
		{
			if (causal_clock.active_writer_ts[chan][s] == PG_INT64_MAX)
			{
				slot = s;
				break;
			}
		}
	}
	causal_clock.active_writer_ts[chan][slot] = event_ts;
	cur_wm = __atomic_load_n(&causal_clock.channel_watermarks[chan], __ATOMIC_RELAXED);
	unlockCausalClock();

	if (st != NULL)
	{
		st->has_active_dep_write = true;
		st->active_dep_write_chan = chan;
	}

	return cur_wm;
}

/*
 * Complete an in-flight dependent write for client st on channel chan,
 * advancing max_committed_ts[chan], clearing the client's active slot,
 * and advancing the safe watermark to min(max_committed_ts, min_active - 1).
 */
int64
endDependentWriteChan(CState *st, int chan, int64 event_ts)
{
	int			slot = (st != NULL && st->id >= 0) ? (st->id % MAX_WRITER_SLOTS) : 0;
	bool		advanced = false;
	int64		new_wm;
	bool		any_remaining = false;

	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;

	lockCausalClock();
	if (event_ts > causal_clock.max_committed_ts[chan])
		causal_clock.max_committed_ts[chan] = event_ts;

	if (causal_clock.active_writer_ts[chan][slot] == event_ts)
	{
		causal_clock.active_writer_ts[chan][slot] = PG_INT64_MAX;
	}
	else
	{
		bool		cleared = false;

		for (int s = 0; s < MAX_WRITER_SLOTS; s++)
		{
			if (causal_clock.active_writer_ts[chan][s] == event_ts)
			{
				causal_clock.active_writer_ts[chan][s] = PG_INT64_MAX;
				cleared = true;
				break;
			}
		}
		if (!cleared)
			causal_clock.active_writer_ts[chan][slot] = PG_INT64_MAX;
	}

	if (causal_clock.active_writer_ts[chan][slot] != PG_INT64_MAX)
		any_remaining = true;

	new_wm = recomputeSafeWatermarkLocked(chan, &advanced);
	unlockCausalClock();

	if (st != NULL && st->has_active_dep_write && st->active_dep_write_chan == chan && !any_remaining)
		st->has_active_dep_write = false;

	if (advanced)
		wakeWatermarkWaiters();

	return new_wm;
}

/*
 * Clear an active dependent write slot without advancing max_committed_ts
 * (invoked automatically if a client aborts, retries, fails, or finishes a
 * transaction while holding an active dependent write slot).
 */
void
cancelDependentWrite(CState *st)
{
	int			chan;
	int			slot;
	bool		advanced = false;

	if (st == NULL || !st->has_active_dep_write)
		return;

	chan = st->active_dep_write_chan;
	if (chan < 0 || chan >= PGBENCH_MAX_WATERMARK_CHANNELS)
		chan = 0;
	slot = (st->id >= 0) ? (st->id % MAX_WRITER_SLOTS) : 0;

	lockCausalClock();
	causal_clock.active_writer_ts[chan][slot] = PG_INT64_MAX;
	(void) recomputeSafeWatermarkLocked(chan, &advanced);
	unlockCausalClock();

	st->has_active_dep_write = false;
	if (advanced)
		wakeWatermarkWaiters();
}

/*
 * Non-blocking cross-client phase barrier helpers.
 */
static inline int
getBarrierSlot(CState *st, int *total_clients_out)
{
	if (st != NULL && st->group_id >= 0 && st->group_id < num_groups &&
		(st->group_id + 1) < MAX_BARRIER_SLOTS)
	{
		if (total_clients_out != NULL)
			*total_clients_out = client_groups[st->group_id].nclients;
		return st->group_id + 1;
	}
	if (total_clients_out != NULL)
		*total_clients_out = nclients;
	return 0;
}

bool
enterPhaseBarrier(CState *st)
{
	int			total_clients = 1;
	int			b = getBarrierSlot(st, &total_clients);
	int			active_target;

	lockCausalClock();
	active_target = total_clients - causal_clock.barrier_done[b];
	if (active_target < 1)
		active_target = 1;

	causal_clock.barrier_arrived[b]++;
	if (causal_clock.barrier_arrived[b] >= active_target)
	{
		causal_clock.barrier_arrived[b] = 0;
		__atomic_store_n(&causal_clock.barrier_generation[b],
						 causal_clock.barrier_generation[b] + 1,
						 __ATOMIC_RELEASE);
		unlockCausalClock();
		wakeWatermarkWaiters();
		return true;
	}
	else
	{
		if (st != NULL)
			st->barrier_gen = causal_clock.barrier_generation[b];
		unlockCausalClock();
		return false;
	}
}

bool
checkPhaseBarrierReady(CState *st)
{
	int			b = getBarrierSlot(st, NULL);
	uint64		cur_gen = __atomic_load_n(&causal_clock.barrier_generation[b],
										  __ATOMIC_ACQUIRE);

	return (st == NULL || cur_gen > st->barrier_gen);
}

void
notifyPhaseBarrierClientDone(CState *st)
{
	int			total_clients = 1;
	int			b;
	int			active_target;
	bool		should_wake = false;

	if (st == NULL || st->barrier_done_notified)
		return;
	st->barrier_done_notified = true;

	b = getBarrierSlot(st, &total_clients);

	lockCausalClock();
	causal_clock.barrier_done[b]++;
	active_target = total_clients - causal_clock.barrier_done[b];
	if (causal_clock.barrier_arrived[b] > 0 &&
		causal_clock.barrier_arrived[b] >= active_target)
	{
		causal_clock.barrier_arrived[b] = 0;
		__atomic_store_n(&causal_clock.barrier_generation[b],
						 causal_clock.barrier_generation[b] + 1,
						 __ATOMIC_RELEASE);
		should_wake = true;
	}
	unlockCausalClock();

	if (should_wake)
		wakeWatermarkWaiters();
}

/*
 * Handler for \wait_until <offset_us_expr> [<dep_watermark_expr>]
 */
ConnectionStateEnum
handle_cmd_wait_until(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue offset_val;
	int64		offset_us = 0;
	double		speed = (replay_speed > 0.0) ? replay_speed : 1.0;

	if (!evaluateExpr(st, cmd->expr, &offset_val))
	{
		commandFailed(st, cmd->argv[0], "evaluation of offset expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&offset_val, &offset_us))
	{
		freeValue(&offset_val);
		commandFailed(st, cmd->argv[0], "offset expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&offset_val);

	if (offset_us >= 0)
	{
		pg_time_usec_t base_us = (st->bench_start_us > 0) ? st->bench_start_us : st->txn_begin;
		pg_time_usec_t scaled_us = (pg_time_usec_t) ((double) offset_us / speed + 0.5);

		st->sync_scheduled_us = base_us + scaled_us;
		st->txn_scheduled = st->sync_scheduled_us;
		st->has_scheduled_time = true;
	}
	else
	{
		st->sync_scheduled_us = 0;
	}

	if (cmd->end_expr != NULL)
	{
		PgBenchValue dep_val;
		int64		dep_wm = 0;

		if (!evaluateExpr(st, cmd->end_expr, &dep_val))
		{
			commandFailed(st, cmd->argv[0], "evaluation of watermark dependency expression failed");
			return CSTATE_ABORTED;
		}
		if (!coerceToInt(&dep_val, &dep_wm))
		{
			freeValue(&dep_val);
			commandFailed(st, cmd->argv[0], "watermark dependency expression is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&dep_val);

		st->has_sync_dep = true;
		st->sync_dep_watermark = dep_wm;
		st->sync_dep_channel = 0;

		if (cmd->step_expr != NULL)
		{
			PgBenchValue chan_val;
			int64		chan = 0;

			if (!evaluateExpr(st, cmd->step_expr, &chan_val) ||
				!coerceToInt(&chan_val, &chan))
			{
				freeValue(&chan_val);
				commandFailed(st, cmd->argv[0], "watermark channel expression is not an integer");
				return CSTATE_ABORTED;
			}
			freeValue(&chan_val);
			st->sync_dep_channel = (int) chan;
		}
	}
	else
	{
		st->has_sync_dep = false;
		st->sync_dep_watermark = 0;
		st->sync_dep_channel = 0;
	}

	return CSTATE_WAIT_SYNC;
}

/*
 * Handler for \advance_watermark <watermark_expr>
 */
ConnectionStateEnum
handle_cmd_advance_watermark(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue wm_val;
	int64		wm = 0;
	int64		chan = 0;

	if (!evaluateExpr(st, cmd->expr, &wm_val))
	{
		commandFailed(st, cmd->argv[0], "evaluation of watermark expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&wm_val, &wm))
	{
		freeValue(&wm_val);
		commandFailed(st, cmd->argv[0], "watermark expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&wm_val);

	if (cmd->end_expr != NULL)
	{
		PgBenchValue chan_val;

		if (!evaluateExpr(st, cmd->end_expr, &chan_val) ||
			!coerceToInt(&chan_val, &chan))
		{
			freeValue(&chan_val);
			commandFailed(st, cmd->argv[0], "watermark channel expression is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&chan_val);
	}

	(void) advanceWatermarkChan((int) chan, wm);
	return CSTATE_END_COMMAND;
}

/*
 * Handler for \begin_dependent_write <ts_expr> [CHANNEL <chan_expr>]
 */
ConnectionStateEnum
handle_cmd_begin_dependent_write(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue ts_val;
	int64		ts = 0;
	int64		chan = 0;

	if (!evaluateExpr(st, cmd->expr, &ts_val))
	{
		commandFailed(st, cmd->argv[0], "evaluation of dependent write timestamp expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&ts_val, &ts))
	{
		freeValue(&ts_val);
		commandFailed(st, cmd->argv[0], "dependent write timestamp expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&ts_val);

	if (cmd->end_expr != NULL)
	{
		PgBenchValue chan_val;

		if (!evaluateExpr(st, cmd->end_expr, &chan_val) ||
			!coerceToInt(&chan_val, &chan))
		{
			freeValue(&chan_val);
			commandFailed(st, cmd->argv[0], "watermark channel expression is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&chan_val);
	}

	(void) beginDependentWriteChan(st, (int) chan, ts);
	return CSTATE_END_COMMAND;
}

/*
 * Handler for \end_dependent_write <ts_expr> [CHANNEL <chan_expr>]
 */
ConnectionStateEnum
handle_cmd_end_dependent_write(CState *st, Command *cmd, pg_time_usec_t *now)
{
	PgBenchValue ts_val;
	int64		ts = 0;
	int64		chan = 0;

	if (!evaluateExpr(st, cmd->expr, &ts_val))
	{
		commandFailed(st, cmd->argv[0], "evaluation of dependent write timestamp expression failed");
		return CSTATE_ABORTED;
	}
	if (!coerceToInt(&ts_val, &ts))
	{
		freeValue(&ts_val);
		commandFailed(st, cmd->argv[0], "dependent write timestamp expression is not an integer");
		return CSTATE_ABORTED;
	}
	freeValue(&ts_val);

	if (cmd->end_expr != NULL)
	{
		PgBenchValue chan_val;

		if (!evaluateExpr(st, cmd->end_expr, &chan_val) ||
			!coerceToInt(&chan_val, &chan))
		{
			freeValue(&chan_val);
			commandFailed(st, cmd->argv[0], "watermark channel expression is not an integer");
			return CSTATE_ABORTED;
		}
		freeValue(&chan_val);
	}

	(void) endDependentWriteChan(st, (int) chan, ts);
	return CSTATE_END_COMMAND;
}

/*
 * Handler for \phase_barrier [<name>]
 */
ConnectionStateEnum
handle_cmd_phase_barrier(CState *st, Command *cmd, pg_time_usec_t *now)
{
	if (enterPhaseBarrier(st))
		return CSTATE_END_COMMAND;
	return CSTATE_WAIT_PHASE_BARRIER;
}

/*-------------------------------------------------------------------------
 * Pillar 3: Shared Entity Reservoirs (Bounded Ring Buffers)
 *-------------------------------------------------------------------------
 */

PgBenchReservoir *
findReservoir(const char *name)
{
	if (name == NULL)
		return NULL;

	for (int i = 0; i < num_reservoirs; i++)
	{
		if (strcmp(reservoirs[i].name, name) == 0)
			return &reservoirs[i];
	}
	return NULL;
}

void
registerReservoir(const char *name, uint64 capacity)
{
	PgBenchReservoir *res;

	if (name == NULL || *name == '\0')
		pg_fatal("reservoir name cannot be empty");
	if (capacity == 0 || capacity > (100ULL * 1000 * 1000))
		pg_fatal("reservoir \"%s\": invalid capacity " UINT64_FORMAT " (must be 1..100000000)",
				 name, capacity);

	if (findReservoir(name) != NULL)
		pg_fatal("duplicate reservoir name \"%s\"", name);
	if (num_reservoirs >= MAX_RESERVOIRS)
		pg_fatal("too many reservoirs registered (maximum %d)", MAX_RESERVOIRS);

	if (!atexit_registered)
	{
		atexit(freeAllDatasets);
		atexit_registered = true;
	}

	res = &reservoirs[num_reservoirs++];
	memset(res, 0, sizeof(PgBenchReservoir));
	res->name = pg_strdup(name);
	res->capacity = capacity;
	res->slots = pg_malloc0_array(int64, capacity);
	__atomic_store_n(&res->write_idx, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&res->valid_count, 0, __ATOMIC_RELAXED);
}

void
parseReservoirSpec(const char *spec)
{
	const char *sep = NULL;
	char	   *name;
	const char *cap_str;
	int64		cap = 0;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --reservoir specification: empty string");

	for (const char *p = spec; *p != '\0'; p++)
	{
		if (*p == ':' || *p == '=')
		{
			sep = p;
			break;
		}
	}

	if (sep == NULL || sep == spec || *(sep + 1) == '\0')
		pg_fatal("invalid --reservoir specification \"%s\" (expected <name>:<capacity>)",
				 spec);

	name = pg_malloc((sep - spec) + 1);
	memcpy(name, spec, sep - spec);
	name[sep - spec] = '\0';
	cap_str = sep + 1;

	if (!strtoint64(cap_str, true, &cap) || cap <= 0)
	{
		pg_free(name);
		pg_fatal("invalid capacity \"%s\" in --reservoir=\"%s\"", cap_str, spec);
	}

	registerReservoir(name, (uint64) cap);
	pg_free(name);
}

bool
reservoirPush(const char *name, int64 val)
{
	PgBenchReservoir *res = findReservoir(name);
	uint64		idx;

	if (res == NULL)
	{
		pg_log_error("unknown reservoir \"%s\"", name ? name : "");
		return false;
	}

	idx = __atomic_fetch_add(&res->write_idx, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&res->slots[idx % res->capacity], val, __ATOMIC_RELEASE);

	if (idx < res->capacity)
	{
		while (__atomic_load_n(&res->valid_count, __ATOMIC_ACQUIRE) < idx)
		{
			/* Wait for preceding initial slot writes so slots[0..idx] are valid */
		}
		__atomic_store_n(&res->valid_count, idx + 1, __ATOMIC_RELEASE);
	}

	return true;
}

/*
 * Push a scalar integer or all elements of a PGBT_ARRAY of integers into a
 * reservoir.
 */
bool
reservoirPushValue(const char *name, const PgBenchValue *val, int64 *out_res)
{
	if (findReservoir(name) == NULL)
	{
		pg_log_error("unknown reservoir \"%s\"", name ? name : "");
		return false;
	}

	if (val == NULL || val->type == PGBT_NULL)
	{
		if (out_res != NULL)
			*out_res = 0;
		return true;
	}

	if (val->type == PGBT_ARRAY)
	{
		int64		pushed = 0;

		for (int i = 0; i < val->u.aval.nelems; i++)
		{
			const PgBenchValue *elem = &val->u.aval.elems[i];
			int64		ival = 0;

			if (elem->type == PGBT_NULL)
				continue;
			if (!coerceToInt(elem, &ival))
				return false;
			if (!reservoirPush(name, ival))
				return false;
			pushed++;
		}
		if (out_res != NULL)
			*out_res = pushed;
		return true;
	}
	else
	{
		int64		ival = 0;

		if (!coerceToInt(val, &ival))
			return false;
		if (!reservoirPush(name, ival))
			return false;
		if (out_res != NULL)
			*out_res = ival;
		return true;
	}
}

bool
reservoirSample(const char *name, pg_prng_state *rstate,
				int64 *out_val, bool *is_empty)
{
	PgBenchReservoir *res = findReservoir(name);
	uint64		cnt;
	int64		pos;

	if (res == NULL)
	{
		pg_log_error("unknown reservoir \"%s\"", name ? name : "");
		return false;
	}

	cnt = __atomic_load_n(&res->valid_count, __ATOMIC_ACQUIRE);
	if (cnt == 0)
	{
		*is_empty = true;
		*out_val = 0;
		return true;
	}

	*is_empty = false;
	pos = pgbench_random(rstate, 0, (int64) cnt - 1);
	*out_val = __atomic_load_n(&res->slots[pos], __ATOMIC_ACQUIRE);
	return true;
}

int64
reservoirCount(const char *name)
{
	PgBenchReservoir *res = findReservoir(name);

	if (res == NULL)
		return -1;
	return (int64) __atomic_load_n(&res->valid_count, __ATOMIC_ACQUIRE);
}

/*
 * Handler for \reservoir_push <name> <expr>
 */
ConnectionStateEnum
handle_cmd_reservoir_push(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *res_name = cmd->argv[1];
	PgBenchValue v;
	int64		out_res = 0;
	bool		ok;

	if (!evaluateExpr(st, cmd->expr, &v))
	{
		commandFailed(st, cmd->argv[0], "evaluation of reservoir_push expression failed");
		return CSTATE_ABORTED;
	}

	ok = reservoirPushValue(res_name, &v, &out_res);
	freeValue(&v);

	if (!ok)
	{
		commandFailed(st, cmd->argv[0], "reservoir_push failed (expected integer or integer array)");
		return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}


/*
 * Handler for \reservoir_sample <name> <varname> [<default_expr>]
 */
ConnectionStateEnum
handle_cmd_reservoir_sample(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *res_name = cmd->argv[1];
	const char *varname = normalizeVarArg(cmd->argv[2]);
	int64		sampled_val = 0;
	bool		is_empty = false;

	if (!reservoirSample(res_name, &st->cs_func_rs, &sampled_val, &is_empty))
	{
		commandFailed(st, cmd->argv[0], "reservoir_sample failed");
		return CSTATE_ABORTED;
	}

	if (is_empty)
	{
		if (cmd->expr != NULL)
		{
			PgBenchValue def_v;
			bool		ok;

			if (!evaluateExpr(st, cmd->expr, &def_v))
			{
				commandFailed(st, cmd->argv[0], "evaluation of default expression failed");
				return CSTATE_ABORTED;
			}
			ok = putVariableValue(&st->variables, "reservoir_sample", (char *) varname, &def_v);
			freeValue(&def_v);
			if (!ok)
				return CSTATE_ABORTED;
		}
		else
		{
			PgBenchValue null_v;

			setNullValue(&null_v);
			if (!putVariableValue(&st->variables, "reservoir_sample", (char *) varname, &null_v))
				return CSTATE_ABORTED;
		}
	}
	else
	{
		if (!putVariableInt(&st->variables, "reservoir_sample", (char *) varname, sampled_val))
			return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}

