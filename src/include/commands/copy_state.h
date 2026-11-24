/*-------------------------------------------------------------------------
 *
 * copy_state.h
 *	  prototypes for COPY TO/COPY FROM execution state.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/commands/copy_state.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COPY_STATE_H
#define COPY_STATE_H

#include "commands/copy.h"
#include "commands/trigger.h"
#include "executor/execdesc.h"
#include "nodes/miscnodes.h"

/*
 * Represents the different source cases we need to worry about at
 * the bottom level
 */
typedef enum CopySource
{
	COPY_SOURCE_FILE,			/* from file (or a piped program) */
	COPY_SOURCE_FRONTEND,		/* from frontend */
	COPY_SOURCE_CALLBACK,		/* from callback function */
} CopySource;

/*
 *	Represents the end-of-line terminator type of the input
 */
typedef enum EolType
{
	EOL_UNKNOWN,
	EOL_NL,
	EOL_CR,
	EOL_CRNL,
} EolType;

/*
 * This struct contains all the state variables used throughout a COPY FROM
 * operation.
 *
 * The fields in the first section are available to extensions implementing a
 * COPY format.  The others are internal to core and the built-in formats, and
 * an extension must not use them; it uses the functions declared in
 * copyapi.h instead.
 *
 * So that extensions keep working across minor releases, the fields in the
 * first section must not be changed in a stable branch, and a field added
 * there must go at the end of the struct.
 */
typedef struct CopyFromStateData
{
	/*
	 * Fields available to extensions.  rel, attnumlist and opts are read
	 * only.
	 */
	Relation	rel;			/* relation to copy from */
	List	   *attnumlist;		/* integer list of attnums to copy */
	CopyFormatOptions opts;
	MemoryContext copycontext;	/* per-copy execution context */
	void	   *format_private; /* custom format's own state */

	/*
	 * Internal fields
	 */

	/* format routine */
	const struct CopyFromRoutine *routine;

	/* low-level state data */
	CopySource	copy_src;		/* type of copy source */
	FILE	   *copy_file;		/* used if copy_src == COPY_SOURCE_FILE */
	StringInfo	fe_msgbuf;		/* used if copy_src == COPY_SOURCE_FRONTEND */

	EolType		eol_type;		/* EOL type of input */
	int			file_encoding;	/* file or remote side's character encoding */
	bool		need_transcoding;	/* file encoding diff from server? */
	Oid			conversion_proc;	/* encoding conversion function */

	/* parameters from the COPY command */
	char	   *filename;		/* filename, or NULL for STDIN */
	bool		is_program;		/* is 'filename' a program to popen? */
	copy_data_source_cb data_source_cb; /* function for reading data */

	bool	   *convert_select_flags;	/* per-column CSV/TEXT CS flags */
	Node	   *whereClause;	/* WHERE condition (or NULL) */

	/* these are just for error reporting, see CopyFromErrorCallback */
	const char *cur_relname;	/* table name for error messages */
	uint64		cur_lineno;		/* line number for error messages */
	const char *cur_attname;	/* current att for error messages */
	const char *cur_attval;		/* current att value for error messages */
	bool		cur_attval_built;	/* att value was built by a custom format */
	bool		relname_only;	/* don't output line number, att, etc. */

	/*
	 * Working state
	 */
	AttrNumber	num_defaults;	/* count of att that are missing and have
								 * default value */
	FmgrInfo   *in_functions;	/* array of input functions for each attrs */
	Oid		   *typioparams;	/* array of element types for in_functions */
	FunctionCallInfo *in_fcinfos;	/* call info for in_functions, set up
									 * once; NULL in binary format */
	ErrorSaveContext *escontext;	/* soft error trapped during in_functions
									 * execution */
	uint64		num_errors;		/* total number of rows which contained soft
								 * errors */
	bool		cur_row_erroneous;	/* ON_ERROR SET_NULL has counted the
									 * current row in num_errors; reset by
									 * core at the start of each row */
	int		   *defmap;			/* array of default att numbers related to
								 * missing att */
	ExprState **defexprs;		/* array of default att expressions for all
								 * att */
	bool	   *defaults;		/* if DEFAULT marker was found for
								 * corresponding att */

	/*
	 * Arrays for the per-row callback of a custom format to fill, one element
	 * for each column of attnumlist.  NULL if attnumlist is all the
	 * attributes in order, in which case the callback fills the arrays of the
	 * whole row directly, and for the built-in formats.
	 */
	Datum	   *row_values;
	bool	   *row_nulls;

	/*
	 * Per-row state of a custom format, reset for each row; see
	 * CopyFromConvertField().
	 */
	bool		cur_row_skip;	/* ON_ERROR IGNORE dropped the row */
	bool		cur_row_has_default;	/* a column is set to its default */

	/*
	 * Columns of a custom format that need their typmod or domain constraints
	 * applied to the values the format builds by itself, and which columns of
	 * the current row core has set instead.  See CopyFromCoerceDatums().
	 */
	struct CopyFromCoerceColumn *coerce_columns;
	int			num_coerce_columns;
	bool	   *row_set_by_core;	/* one for each column of attnumlist */

	bool		simd_enabled;	/* use SIMD to scan for special chars? */

	/*
	 * True if the corresponding attribute's is a constrained domain. This
	 * will be populated only when ON_ERROR is SET_NULL, otherwise NULL.
	 */
	bool	   *domain_with_constraint;

	bool		volatile_defexprs;	/* is any of defexprs volatile? */
	List	   *range_table;	/* single element list of RangeTblEntry */
	List	   *rteperminfos;	/* single element list of RTEPermissionInfo */
	ExprState  *qualexpr;

	TransitionCaptureState *transition_capture;

	/*
	 * These variables are used to reduce overhead in COPY FROM.
	 *
	 * attribute_buf holds the separated, de-escaped text for each field of
	 * the current line.  The CopyReadAttributes functions return arrays of
	 * pointers into this buffer.  We avoid palloc/pfree overhead by re-using
	 * the buffer on each cycle.
	 *
	 * In binary COPY FROM, attribute_buf holds the binary data for the
	 * current field, but the usage is otherwise similar.
	 */
	StringInfoData attribute_buf;

	/* field raw data pointers found by COPY FROM */

	int			max_fields;
	char	  **raw_fields;

	/*
	 * Similarly, line_buf holds the whole input line being processed. The
	 * input cycle is first to read the whole line into line_buf, and then
	 * extract the individual attribute fields into attribute_buf.  line_buf
	 * is preserved unmodified so that we can display it in error messages if
	 * appropriate.  (In binary mode, line_buf is not used.)
	 */
	StringInfoData line_buf;
	bool		line_buf_valid; /* contains the row being processed? */

	/*
	 * input_buf holds input data, already converted to database encoding.
	 *
	 * In text mode, CopyReadLine parses this data sufficiently to locate line
	 * boundaries, then transfers the data to line_buf. We guarantee that
	 * there is a \0 at input_buf[input_buf_len] at all times.  (In binary
	 * mode, input_buf is not used.)
	 *
	 * If encoding conversion is not required, input_buf is not a separate
	 * buffer but points directly to raw_buf.  In that case, input_buf_len
	 * tracks the number of bytes that have been verified as valid in the
	 * database encoding, and raw_buf_len is the total number of bytes stored
	 * in the buffer.
	 */
#define INPUT_BUF_SIZE 65536	/* we palloc INPUT_BUF_SIZE+1 bytes */
	char	   *input_buf;
	int			input_buf_index;	/* next byte to process */
	int			input_buf_len;	/* total # of bytes stored */
	bool		input_reached_eof;	/* true if we reached EOF */
	bool		input_reached_error;	/* true if a conversion error happened */
	/* Shorthand for number of unconsumed bytes available in input_buf */
#define INPUT_BUF_BYTES(cstate) ((cstate)->input_buf_len - (cstate)->input_buf_index)

	/*
	 * raw_buf holds raw input data read from the data source (file or client
	 * connection), not yet converted to the database encoding.  Like with
	 * 'input_buf', we guarantee that there is a \0 at raw_buf[raw_buf_len].
	 */
#define RAW_BUF_SIZE 65536		/* we palloc RAW_BUF_SIZE+1 bytes */
	char	   *raw_buf;
	int			raw_buf_index;	/* next byte to process */
	int			raw_buf_len;	/* total # of bytes stored */
	bool		raw_reached_eof;	/* true if we reached EOF */

	/* Shorthand for number of unconsumed bytes available in raw_buf */
#define RAW_BUF_BYTES(cstate) ((cstate)->raw_buf_len - (cstate)->raw_buf_index)

	uint64		bytes_processed;	/* number of bytes processed so far */
} CopyFromStateData;

/*
 * Represents the different dest cases we need to worry about at
 * the bottom level
 */
typedef enum CopyDest
{
	COPY_DEST_FILE,				/* to file (or a piped program) */
	COPY_DEST_FRONTEND,			/* to frontend */
	COPY_DEST_CALLBACK,			/* to callback function */
} CopyDest;

/*
 * This struct contains all the state variables used throughout a COPY TO
 * operation.
 *
 * Multi-byte encodings: all supported client-side encodings encode multi-byte
 * characters by having the first byte's high bit set. Subsequent bytes of the
 * character can have the high bit not set. When scanning data in such an
 * encoding to look for a match to a single-byte (ie ASCII) character, we must
 * use the full pg_encoding_mblen() machinery to skip over multibyte
 * characters, else we might find a false match to a trailing byte. In
 * supported server encodings, there is no possibility of a false match, and
 * it's faster to make useless comparisons to trailing bytes than it is to
 * invoke pg_encoding_mblen() to skip over them. encoding_embeds_ascii is true
 * when we have to do it the hard way.
 *
 * As in CopyFromStateData, the fields in the first section are available to
 * extensions, and must not be changed in a stable branch.
 */
typedef struct CopyToStateData
{
	/*
	 * Fields available to extensions.  rel, attnumlist and opts are read
	 * only; rel is NULL for COPY (query) TO.  out_functions holds the text
	 * output functions for a custom format.
	 */
	Relation	rel;			/* relation to copy to */
	List	   *attnumlist;		/* integer list of attnums to copy */
	CopyFormatOptions opts;
	MemoryContext copycontext;	/* per-copy execution context */
	FmgrInfo   *out_functions;	/* lookup info for output functions */
	void	   *format_private; /* custom format's own state */

	/*
	 * Internal fields
	 */

	/* format-specific routines */
	const struct CopyToRoutine *routine;

	/* low-level state data */
	CopyDest	copy_dest;		/* type of copy source/destination */
	FILE	   *copy_file;		/* used if copy_dest == COPY_DEST_FILE */
	StringInfo	fe_msgbuf;		/* used for all dests during COPY TO */

	int			file_encoding;	/* file or remote side's character encoding */
	bool		need_transcoding;	/* file encoding diff from server? */
	bool		encoding_embeds_ascii;	/* ASCII can be non-first byte? */

	/* parameters from the COPY command */
	QueryDesc  *queryDesc;		/* executable query to copy from */
	char	   *filename;		/* filename, or NULL for STDOUT */
	bool		is_program;		/* is 'filename' a program to popen? */
	bool		json_row_delim_needed;	/* need delimiter before next row */
	StringInfo	json_buf;		/* reusable buffer for JSON output,
								 * initialized in BeginCopyTo */
	TupleDesc	tupDesc;		/* Descriptor for JSON output; for a column
								 * list this is a projected descriptor */
	Datum	   *json_projvalues;	/* pre-allocated projection values, or
									 * NULL */
	bool	   *json_projnulls; /* pre-allocated projection nulls, or NULL */
	copy_data_dest_cb data_dest_cb; /* function for writing data */

	Node	   *whereClause;	/* WHERE condition (or NULL) */
	List	   *partitions;		/* OID list of partitions to copy data from */

	/*
	 * Working state
	 */
	MemoryContext rowcontext;	/* per-row evaluation context */
	uint64		bytes_processed;	/* number of bytes processed so far */
} CopyToStateData;

#endif							/* COPY_STATE_H */
