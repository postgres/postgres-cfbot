/*-------------------------------------------------------------------------
 *
 * copy_fixed.c
 *		A fixed-width COPY TO/FROM format.
 *
 * In a fixed-width file every column occupies a fixed number of bytes,
 * padded out with a fill character; there is no delimiter between columns.
 * Field widths are counted in bytes of the *file* encoding, because the
 * layout is a property of the file, not of the server.
 *
 * That is why COPY FROM here reads the source with CopyFromGetData() and
 * converts each field on its own, rather than reading whole lines.  The
 * core line-reading machinery hands back data that has already been
 * converted to the server encoding, where the byte offsets no longer
 * correspond to the layout in the file.
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/copy_fixed/copy_fixed.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/copy.h"
#include "commands/copyapi.h"
#include "commands/copy_state.h"
#include "commands/defrem.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
					.name = "copy_fixed",
					.version = PG_VERSION
);

typedef enum FixedAlign
{
	FIXED_ALIGN_LEFT,
	FIXED_ALIGN_RIGHT,
} FixedAlign;

typedef enum FixedRecordDelim
{
	FIXED_DELIM_LF,
	FIXED_DELIM_CRLF,
	FIXED_DELIM_NONE,
} FixedRecordDelim;

/*
 * Format-specific options, hung off CopyFormatOptions.format_private_opts.
 *
 * This lives only as long as the memory context that was current while
 * ProcessCopyOptions() ran -- file_fdw, for one, throws the whole
 * CopyFormatOptions away right after validating it -- so nothing may keep a
 * reference to it once the COPY itself has started.  FixedCopyState takes
 * its own copy of everything it needs.
 *
 * The *_specified flags exist because every one of "pad", "align" and
 * "record_delimiter" has a legitimate default that is indistinguishable
 * from an unset value (e.g. a user-supplied "align 'left'" looks the same
 * as never having set align at all), so redundant-option detection needs
 * its own bit per option rather than being able to test the value itself.
 * "widths" doesn't need one because NULL is not a value it can otherwise
 * take.
 */
typedef struct FixedFormatOptions
{
	int		   *widths;			/* NULL until "widths" has been seen */
	int			nwidths;
	char		pad;
	bool		pad_specified;
	FixedAlign	align;
	bool		align_specified;
	FixedRecordDelim record_delim;
	bool		record_delim_specified;
} FixedFormatOptions;

/*
 * Per-COPY working state, hung off Copy{To,From}StateData.format_private and
 * allocated in cstate->copycontext.
 */
typedef struct FixedCopyState
{
	int		   *widths;			/* one per entry of cstate->attnumlist */
	int			nwidths;
	char		pad;
	FixedAlign	align;
	FixedRecordDelim record_delim;
	int			total_width;	/* sum of widths; excludes the delimiter */

	StringInfoData fieldbuf;	/* COPY TO only: scratch space for one field */

	/* COPY TO only: the NULL marker converted to the file encoding */
	const char *null_print;
	int			null_print_len;

	/*
	 * COPY FROM only.  recbuf holds the bytes of one record exactly as they
	 * appear in the file; recordbuf holds the same record's fields once they
	 * have been converted to the server encoding and stripped of their
	 * padding, each one NUL-terminated.
	 *
	 * The fields need a buffer of their own, since converting a field to the
	 * server encoding can change its length and each field must be
	 * NUL-terminated.  recordbuf lives in cstate->copycontext and is reset
	 * per record, like cstate->attribute_buf for the built-in formats.
	 *
	 * Appending to recordbuf can repalloc it and move it, so field_off
	 * records where each field begins as an offset rather than as a pointer;
	 * the offsets are resolved only once the whole record has been assembled.
	 * A field that matched the NULL marker gets an offset of -1.
	 */
	char	   *recbuf;			/* total_width + 1 bytes */
	StringInfoData recordbuf;	/* one record's fields, NUL-terminated */
	int		   *field_off;		/* one entry per field; -1 means NULL */
	char	  **fields;			/* one entry per field, passed to core */
	bool	   *field_isdefault;	/* one entry per field */
} FixedCopyState;

/* Number of bytes the record delimiter occupies */
static inline int
fixed_delim_len(FixedRecordDelim d)
{
	switch (d)
	{
		case FIXED_DELIM_LF:
			return 1;
		case FIXED_DELIM_CRLF:
			return 2;
		case FIXED_DELIM_NONE:
			return 0;
	}
	return 0;					/* keep compiler quiet */
}

/*
 * Fetch our option struct, creating it with the format's defaults on first
 * use.  ProcessCopyOptions() calls the option callback only for options core
 * did not recognize, so this must also be reachable from the validation
 * callback, which runs even when no format-specific option was given.
 */
static FixedFormatOptions *
fixed_get_opts(CopyFormatOptions *opts)
{
	FixedFormatOptions *fopts = (FixedFormatOptions *) opts->format_private_opts;

	if (fopts == NULL)
	{
		fopts = palloc0_object(FixedFormatOptions);
		fopts->pad = ' ';
		fopts->align = FIXED_ALIGN_LEFT;
		fopts->record_delim = FIXED_DELIM_LF;
		opts->format_private_opts = (void *) fopts;
	}

	return fopts;
}

/*
 * Parse the "widths" option: a comma-separated list of positive integers,
 * such as '10,5,20'.
 */
static void
fixed_parse_widths(FixedFormatOptions *fopts, const char *str)
{
	List	   *elems = NIL;
	const char *p = str;
	int			i;

	for (;;)
	{
		char	   *end;
		long		val;

		while (*p == ' ')
			p++;

		errno = 0;
		val = strtol(p, &end, 10);
		if (end == p || errno != 0 || val <= 0 || val > PG_INT32_MAX)
			goto invalid;

		elems = lappend_int(elems, (int) val);

		p = end;
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		if (*p != ',')
			goto invalid;
		p++;
	}

	fopts->nwidths = list_length(elems);
	fopts->widths = palloc_array(int, fopts->nwidths);
	i = 0;
	foreach_int(w, elems)
		fopts->widths[i++] = w;
	list_free(elems);
	return;

invalid:
	ereport(ERROR,
			errcode(ERRCODE_INVALID_PARAMETER_VALUE),
	/*- translator: first %s is the name of a COPY option, e.g. widths,
	 second %s is the invalid value given for it */
			errmsg("invalid value for COPY option \"%s\": \"%s\"",
				   "widths", str),
			errdetail("Widths must be a comma-separated list of positive integers."));
}

/* Complain about an option that was given more than once */
static void
fixed_duplicate_option(const char *name)
{
	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
			errmsg("conflicting or redundant options"),
	/*- translator: %s is the name of a COPY option, e.g. widths */
			errdetail("Option \"%s\" specified more than once.", name));
}

/* Process one option that core did not recognize */
static bool
fixed_process_one_option(CopyFormatOptions *opts, bool is_from, DefElem *option)
{
	FixedFormatOptions *fopts = fixed_get_opts(opts);
	char	   *val;

	if (strcmp(option->defname, "widths") == 0)
	{
		if (fopts->widths != NULL)
			fixed_duplicate_option(option->defname);
		fixed_parse_widths(fopts, defGetString(option));
		return true;
	}
	else if (strcmp(option->defname, "pad") == 0)
	{
		if (fopts->pad_specified)
			fixed_duplicate_option(option->defname);

		val = defGetString(option);
		if (strlen(val) != 1)
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			/*- translator: %s is the name of a COPY option, e.g. pad */
					errmsg("COPY option \"%s\" must be a single one-byte ASCII character",
						   "pad"));
		if (IS_HIGHBIT_SET(val[0]))
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			/*- translator: %s is the name of a COPY option, e.g. pad */
					errmsg("COPY option \"%s\" must be a single one-byte ASCII character",
						   "pad"));
		if (val[0] == '\n' || val[0] == '\r')
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			/*- translator: %s is the name of a COPY option, e.g. pad */
					errmsg("COPY option \"%s\" must be a single one-byte ASCII character",
						   "pad"),
					errdetail("Newline and carriage return are not accepted."));
		fopts->pad = val[0];
		fopts->pad_specified = true;
		return true;
	}
	else if (strcmp(option->defname, "align") == 0)
	{
		if (fopts->align_specified)
			fixed_duplicate_option(option->defname);

		val = defGetString(option);
		if (strcmp(val, "left") == 0)
			fopts->align = FIXED_ALIGN_LEFT;
		else if (strcmp(val, "right") == 0)
			fopts->align = FIXED_ALIGN_RIGHT;
		else
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			/*- translator: %s is the name of a COPY option, e.g. align */
					errmsg("COPY option \"%s\" must be \"left\" or \"right\"",
						   "align"));
		fopts->align_specified = true;
		return true;
	}
	else if (strcmp(option->defname, "record_delimiter") == 0)
	{
		if (fopts->record_delim_specified)
			fixed_duplicate_option(option->defname);

		val = defGetString(option);
		if (strcmp(val, "lf") == 0)
			fopts->record_delim = FIXED_DELIM_LF;
		else if (strcmp(val, "crlf") == 0)
			fopts->record_delim = FIXED_DELIM_CRLF;
		else if (strcmp(val, "none") == 0)
			fopts->record_delim = FIXED_DELIM_NONE;
		else
			ereport(ERROR,
					errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			/*- translator: %s is the name of a COPY option, e.g.
			 record_delimiter */
					errmsg("COPY option \"%s\" must be \"lf\", \"crlf\" or \"none\"",
						   "record_delimiter"));
		fopts->record_delim_specified = true;
		return true;
	}

	return false;
}

/* Reject a core option this format does not implement */
static void
fixed_reject_option(const char *name)
{
	ereport(ERROR,
			errcode(ERRCODE_SYNTAX_ERROR),
	/*- translator: %s is the name of a COPY option, e.g. DELIMITER */
			errmsg("cannot specify %s in the \"fixed\" format", name));
}

/*
 * Validate the fully parsed options.
 *
 * Core doesn't supply DELIMITER and NULL defaults for a custom format, so a
 * non-NULL opts->delim here means the user really wrote it, and we can both
 * reject what we don't implement and pick our own NULL marker.
 */
static void
fixed_validate_options(CopyFormatOptions *opts, bool is_from)
{
	FixedFormatOptions *fopts = fixed_get_opts(opts);

	if (fopts->widths == NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("COPY format \"fixed\" requires the \"widths\" option"));

	/*
	 * Reading strips the padding off each field, and nothing in the file says
	 * where the value ended and the padding began.  That is harmless as long
	 * as the padding character cannot occur in the data, which for a space is
	 * true often enough to be useful and for anything else is not: with "pad
	 * '0'" the value 100 in a width of five is written 10000 and read back as
	 * 1, and a right-aligned 0 in the same field is written 00000, strips to
	 * nothing, and matches the default empty NULL marker.  No stripping rule
	 * recovers the original, since 00100, 100 and 0000100 are the same bytes,
	 * so the combination is refused instead.
	 *
	 * COPY TO keeps accepting any padding character.  Writing a file for
	 * another system to read is precisely what a padding character other than
	 * a space is for.
	 */
	if (is_from && fopts->pad != ' ')
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		/*- translator: %s is the name of a COPY option, e.g. pad */
				errmsg("COPY option \"%s\" must be a space in COPY FROM",
					   "pad"),
				errdetail("Padding cannot be stripped unambiguously, because the padding character can also occur in the data."),
				errhint("Use a space, the default, for a file that has to be read back."));

	if (opts->delim)
		fixed_reject_option("DELIMITER");
	if (opts->quote)
		fixed_reject_option("QUOTE");
	if (opts->escape)
		fixed_reject_option("ESCAPE");
	if (opts->force_quote || opts->force_quote_all)
		fixed_reject_option("FORCE_QUOTE");
	if (opts->force_notnull || opts->force_notnull_all)
		fixed_reject_option("FORCE_NOT_NULL");
	if (opts->force_null || opts->force_null_all)
		fixed_reject_option("FORCE_NULL");
	if (opts->force_array)
		fixed_reject_option("FORCE_ARRAY");
	if (opts->header_line != COPY_HEADER_FALSE)
		fixed_reject_option("HEADER");

	/*
	 * An unpadded field is the natural spelling of NULL in a fixed-width
	 * file, so that is our default.  Core would have defaulted this to \N.
	 */
	if (opts->null_print == NULL)
		opts->null_print = "";
	opts->null_print_len = strlen(opts->null_print);

	/*
	 * A marker containing a newline would break record framing, and core
	 * skips these checks for custom formats.
	 */
	if (strchr(opts->null_print, '\r') != NULL ||
		strchr(opts->null_print, '\n') != NULL)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("COPY null representation cannot use newline or carriage return"));
	if (opts->default_print != NULL &&
		(strchr(opts->default_print, '\r') != NULL ||
		 strchr(opts->default_print, '\n') != NULL))
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("COPY default representation cannot use newline or carriage return"));

	/*
	 * Core's equivalent check only runs for the built-in formats (see
	 * ProcessCopyOptions()), and our own default NULL marker is the empty
	 * string, so "DEFAULT ''" is a real collision that we must catch
	 * ourselves.
	 */
	if (opts->default_print != NULL &&
		opts->null_print_len == opts->default_print_len &&
		strncmp(opts->null_print, opts->default_print,
				opts->null_print_len) == 0)
		ereport(ERROR,
				errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				errmsg("NULL specification and DEFAULT specification cannot be the same"));
}

/*
 * Build the per-COPY state.  'cxt' must be the COPY's own context, since the
 * option struct we read from here does not outlive ProcessCopyOptions().
 */
static FixedCopyState *
fixed_init_state(CopyFormatOptions *opts, List *attnumlist, MemoryContext cxt)
{
	FixedFormatOptions *fopts = (FixedFormatOptions *) opts->format_private_opts;
	MemoryContext oldcxt;
	FixedCopyState *fstate;
	int			natts = list_length(attnumlist);
	int			total = 0;

	Assert(fopts != NULL && fopts->widths != NULL);

	if (fopts->nwidths != natts)
		ereport(ERROR,
				errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				errmsg("COPY format \"fixed\" requires one width per copied column"),
				errdetail("Widths given: %d; columns being copied: %d.",
						  fopts->nwidths, natts));

	for (int i = 0; i < fopts->nwidths; i++)
	{
		if (pg_add_s32_overflow(total, fopts->widths[i], &total))
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("total width of a \"fixed\" format record is too large"));
	}

	oldcxt = MemoryContextSwitchTo(cxt);

	fstate = palloc0_object(FixedCopyState);
	fstate->nwidths = fopts->nwidths;
	fstate->widths = palloc_array(int, fopts->nwidths);
	memcpy(fstate->widths, fopts->widths, fopts->nwidths * sizeof(int));
	fstate->pad = fopts->pad;
	fstate->align = fopts->align;
	fstate->record_delim = fopts->record_delim;
	fstate->total_width = total;
	initStringInfo(&fstate->fieldbuf);

	MemoryContextSwitchTo(oldcxt);

	return fstate;
}

/* Append 'n' copies of the pad character to 'buf' */
static void
fixed_append_pad(StringInfo buf, char pad, int n)
{
	Assert(n >= 0);

	enlargeStringInfo(buf, n);
	memset(buf->data + buf->len, pad, n);
	buf->len += n;
	buf->data[buf->len] = '\0';
}

/*
 * COPY TO callbacks
 */

static void
fixed_CopyToStart(CopyToState cstate, TupleDesc tupDesc)
{
	FixedCopyState *fstate;

	fstate = fixed_init_state(&cstate->opts, cstate->attnumlist,
							  cstate->copycontext);

	/*
	 * The NULL marker goes to the file verbatim, so convert it once here
	 * instead of for every row.  CopyToSendData() applies no conversion.
	 */
	{
		MemoryContext oldcxt = MemoryContextSwitchTo(cstate->copycontext);

		fstate->null_print = CopyToConvertFromServer(cstate,
													 cstate->opts.null_print,
													 cstate->opts.null_print_len);
		MemoryContextSwitchTo(oldcxt);
	}
	fstate->null_print_len = strlen(fstate->null_print);

	cstate->format_private = fstate;
}

static void
fixed_CopyToOneRow(CopyToState cstate, TupleTableSlot *slot)
{
	FixedCopyState *fstate = (FixedCopyState *) cstate->format_private;
	TupleDesc	tupDesc = slot->tts_tupleDescriptor;
	int			i = 0;

	foreach_int(attnum, cstate->attnumlist)
	{
		int			width = fstate->widths[i++];
		const char *value;
		int			len;
		bool		isnull = slot->tts_isnull[attnum - 1];

		if (isnull)
		{
			value = fstate->null_print;
			len = fstate->null_print_len;
		}
		else
		{
			char	   *string;

			string = OutputFunctionCall(&cstate->out_functions[attnum - 1],
										slot->tts_values[attnum - 1]);

			/*
			 * Widths are counted in bytes of the file encoding, so convert
			 * before measuring the length, not after.
			 */
			string = CopyToConvertFromServer(cstate, string, strlen(string));
			value = string;
			len = strlen(string);

			/*
			 * A value containing a newline or carriage return would split the
			 * record across physical lines, corrupting the framing that
			 * record_delimiter relies on to find row boundaries again -- and
			 * a fixed-width format has no escaping mechanism to protect
			 * against that.  With record_delimiter 'none' there is no framing
			 * to break, so allow it there.
			 */
			if (fstate->record_delim != FIXED_DELIM_NONE &&
				(memchr(value, '\n', len) != NULL ||
				 memchr(value, '\r', len) != NULL))
				ereport(ERROR,
						errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						errmsg("value contains a record delimiter character"),
						errdetail("Column \"%s\" contains a newline or carriage return.",
								  NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname)));
		}

		if (len > width)
		{
			if (isnull)
				ereport(ERROR,
						errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("NULL marker too long for fixed-width field"),
						errdetail("Column \"%s\" has a NULL marker that needs %d bytes, but its field width is %d.",
								  NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname),
								  len, width));
			else
				ereport(ERROR,
						errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
						errmsg("value too long for fixed-width field"),
						errdetail("Column \"%s\" needs %d bytes, but its field width is %d.",
								  NameStr(TupleDescAttr(tupDesc, attnum - 1)->attname),
								  len, width));
		}

		resetStringInfo(&fstate->fieldbuf);
		if (fstate->align == FIXED_ALIGN_RIGHT)
			fixed_append_pad(&fstate->fieldbuf, fstate->pad, width - len);
		appendBinaryStringInfo(&fstate->fieldbuf, value, len);
		if (fstate->align == FIXED_ALIGN_LEFT)
			fixed_append_pad(&fstate->fieldbuf, fstate->pad, width - len);

		Assert(fstate->fieldbuf.len == width);
		CopyToSendData(cstate, fstate->fieldbuf.data, width);
	}

	switch (fstate->record_delim)
	{
		case FIXED_DELIM_LF:
			CopyToSendData(cstate, "\n", 1);
			break;
		case FIXED_DELIM_CRLF:
			CopyToSendData(cstate, "\r\n", 2);
			break;
		case FIXED_DELIM_NONE:
			break;
	}

	CopyToFlushData(cstate);
}

static void
fixed_CopyToEnd(CopyToState cstate)
{
	/* Nothing to do here */
}

static const CopyToRoutine FixedCopyToRoutine = {
	.CopyToStart = fixed_CopyToStart,
	.CopyToOneRow = fixed_CopyToOneRow,
	.CopyToEnd = fixed_CopyToEnd,
};

/*
 * COPY FROM callbacks
 */

static void
fixed_CopyFromStart(CopyFromState cstate, TupleDesc tupDesc)
{
	FixedCopyState *fstate;
	MemoryContext oldcxt;

	fstate = fixed_init_state(&cstate->opts, cstate->attnumlist,
							  cstate->copycontext);

	oldcxt = MemoryContextSwitchTo(cstate->copycontext);

	fstate->recbuf = palloc((Size) fstate->total_width + 1);
	fstate->field_off = palloc_array(int, fstate->nwidths);
	fstate->fields = palloc_array(char *, fstate->nwidths);
	fstate->field_isdefault = palloc_array(bool, fstate->nwidths);
	initStringInfo(&fstate->recordbuf);

	MemoryContextSwitchTo(oldcxt);

	cstate->format_private = fstate;
}

/*
 * Consume the record delimiter, if the layout has one.
 *
 * Returns normally at end of input, so that a final record without a
 * trailing delimiter is accepted, as it is for the text format.
 */
static void
fixed_read_record_delimiter(CopyFromState cstate, FixedCopyState *fstate)
{
	char		buf[2];
	int			len = fixed_delim_len(fstate->record_delim);
	int			nread;

	if (len == 0)
		return;

	nread = CopyFromGetData(cstate, buf, len, len);
	if (nread == 0)
		return;					/* no delimiter after the last record */

	if (nread != len ||
		(fstate->record_delim == FIXED_DELIM_LF && buf[0] != '\n') ||
		(fstate->record_delim == FIXED_DELIM_CRLF &&
		 (buf[0] != '\r' || buf[1] != '\n')))
		ereport(ERROR,
				errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				errmsg("expected record delimiter not found"),
		/*- translator: %s is the value of the record_delimiter option */
				errdetail("The \"record_delimiter\" option is \"%s\".",
						  fstate->record_delim == FIXED_DELIM_LF ? "lf" : "crlf"));
}

static bool
fixed_CopyFromOneRow(CopyFromState cstate, ExprContext *econtext,
					 Datum *values, bool *nulls)
{
	FixedCopyState *fstate = (FixedCopyState *) cstate->format_private;
	TupleDesc	tupDesc = RelationGetDescr(cstate->rel);
	int			nread;
	int			off = 0;
	int			i;

	nread = CopyFromGetData(cstate, fstate->recbuf, fstate->total_width,
							fstate->total_width);
	if (nread == 0)
		return false;			/* clean EOF at a record boundary */

	fstate->recbuf[nread] = '\0';

	/* Quote the record in the context of error messages */
	CopyFromSetErrorLine(cstate, fstate->recbuf, nread);

	if (nread < fstate->total_width)
		ereport(ERROR,
				errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				errmsg("incomplete record at end of COPY data"),
				errdetail("Read %d bytes, but the record width is %d.",
						  nread, fstate->total_width));

	/*
	 * Finding a record delimiter among what should have been the record's own
	 * bytes means the record in the file was shorter than the layout says.
	 * Reporting that beats letting the delimiter be read as data and then
	 * failing on a field boundary that has already slipped.
	 */
	if (fstate->record_delim != FIXED_DELIM_NONE &&
		memchr(fstate->recbuf, '\n', nread) != NULL)
		ereport(ERROR,
				errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
				errmsg("record is shorter than the expected width of %d bytes",
					   fstate->total_width));

	fixed_read_record_delimiter(cstate, fstate);

	/*
	 * First pass: convert every field of the record, strip its padding, and
	 * stash the result in recordbuf, remembering where each one landed.  The
	 * pointers to the fields are taken only once the whole record is in
	 * place, because appending to recordbuf may move it.
	 */
	resetStringInfo(&fstate->recordbuf);
	i = 0;
	foreach_int(attnum, cstate->attnumlist)
	{
		int			m = attnum - 1;
		int			width = fstate->widths[i];
		char	   *field;
		int			flen;

		/*
		 * A null byte is a byte like any other in a fixed-width file, but it
		 * cannot be part of a value: the field reaches the input function as
		 * a C string, so everything from the null byte on would be dropped.
		 * Silently truncating what the user is reading in is worse than
		 * refusing to read it.
		 *
		 * CopyFromConvertToServer() would reject it below in any case,
		 * whichever encodings are in play.  Testing for it here costs one
		 * memchr() per field and buys an error that names the offending
		 * column, which an encoding complaint does not.
		 */
		if (memchr(fstate->recbuf + off, '\0', width) != NULL)
			ereport(ERROR,
					errcode(ERRCODE_BAD_COPY_FILE_FORMAT),
					errmsg("null byte in fixed-width field"),
					errdetail("Column \"%s\" contains a null byte.",
							  NameStr(TupleDescAttr(tupDesc, m)->attname)));

		/*
		 * Slice the record on file-encoding byte boundaries, then convert the
		 * field on its own.  CopyFromConvertToServer() also validates the
		 * encoding, which matters because we bypass the core input buffer
		 * that would otherwise have done it.
		 */
		field = CopyFromConvertToServer(cstate, fstate->recbuf + off, width);
		flen = (field == fstate->recbuf + off) ? width : strlen(field);
		off += width;

		/*
		 * Only now is it safe to strip the padding.  In the server encoding
		 * no continuation byte can look like an ASCII pad character, which is
		 * not true of every encoding a file may be in.
		 */
		if (fstate->align == FIXED_ALIGN_RIGHT)
		{
			while (flen > 0 && *field == fstate->pad)
			{
				field++;
				flen--;
			}
		}
		else
		{
			while (flen > 0 && field[flen - 1] == fstate->pad)
				flen--;
		}

		fstate->field_isdefault[i] = false;

		if (flen == cstate->opts.null_print_len &&
			strncmp(field, cstate->opts.null_print, flen) == 0)
		{
			fstate->field_off[i++] = -1;
			continue;
		}

		if (cstate->opts.default_print != NULL &&
			flen == cstate->opts.default_print_len &&
			strncmp(field, cstate->opts.default_print, flen) == 0)
			fstate->field_isdefault[i] = true;

		fstate->field_off[i++] = fstate->recordbuf.len;
		appendBinaryStringInfo(&fstate->recordbuf, field, flen);
		appendStringInfoChar(&fstate->recordbuf, '\0');
	}

	/* Now that recordbuf is done growing, point to the fields in it */
	for (i = 0; i < fstate->nwidths; i++)
		fstate->fields[i] = fstate->field_off[i] < 0 ? NULL :
			fstate->recordbuf.data + fstate->field_off[i];

	/*
	 * Have core convert the fields.  If one is invalid and ON_ERROR is
	 * IGNORE, core drops the row, but we have read the whole record anyway.
	 */
	(void) CopyFromConvertFields(cstate, fstate->fields,
								 fstate->field_isdefault, fstate->nwidths,
								 values, nulls);

	/*
	 * The record stays quoted on the way out, as in the text format, so that
	 * an error raised while the row is being inserted can quote it.
	 */
	return true;
}

static void
fixed_CopyFromEnd(CopyFromState cstate)
{
	/* Nothing to do here */
}

static const CopyFromRoutine FixedCopyFromRoutine = {
	.CopyFromStart = fixed_CopyFromStart,
	.CopyFromOneRow = fixed_CopyFromOneRow,
	.CopyFromEnd = fixed_CopyFromEnd,
};

static const CopyCustomFormatEntry FixedCopyFormat = {
	.name = "fixed",
	.binary = false,
	.to_routine = &FixedCopyToRoutine,
	.from_routine = &FixedCopyFromRoutine,
	.option_fn = fixed_process_one_option,
	.validate_fn = fixed_validate_options,
};

void
_PG_init(void)
{
	RegisterCopyCustomFormat(&FixedCopyFormat);
}
