/*-------------------------------------------------------------------------
 *
 * timeline.c
 *	  timeline-related functions.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <ctype.h>
#include <string.h>

#include "access/timeline.h"
#include "common/pg_parse_lsn.h"
#include "pg_rewind.h"

/*
 * Parse a UUID string in standard dashed form into a pg_uuid_t.
 * Returns true on success, false if str is not a valid UUID string.
 */
static bool
rewind_parse_uuid(const char *str, pg_uuid_t *uuid)
{
	const char *src = str;

	for (int i = 0; i < UUID_LEN; i++)
	{
		char		buf[3];

		if (!isxdigit((unsigned char) src[0]) ||
			!isxdigit((unsigned char) src[1]))
			return false;
		buf[0] = src[0];
		buf[1] = src[1];
		buf[2] = '\0';
		uuid->data[i] = (unsigned char) strtoul(buf, NULL, 16);
		src += 2;
		/* skip dash at positions after bytes 3, 5, 7, 9 (i == 3,5,7,9) */
		if (src[0] == '-' && (i == 3 || i == 5 || i == 7 || i == 9))
			src++;
	}
	return (*src == '\0');
}

/*
 * This is copy-pasted from the backend readTimeLineHistory, modified to
 * return a malloc'd array and to work without backend functions.
 */
/*
 * Try to read a timeline's history file.
 *
 * If successful, return the list of component TLIs (the given TLI followed by
 * its ancestor TLIs).  If we can't find the history file, assume that the
 * timeline has no parents, and return a list of just the specified timeline
 * ID.
 */
TimeLineHistoryEntry *
rewind_parseTimeLineHistory(char *buffer, TimeLineID targetTLI, int *nentries)
{
	char	   *fline;
	TimeLineHistoryEntry *entry;
	TimeLineHistoryEntry *entries = NULL;
	int			nlines = 0;
	TimeLineID	lasttli = 0;
	XLogRecPtr	prevend;
	char	   *bufptr;
	bool		lastline = false;

	/*
	 * Parse the file...
	 */
	prevend = InvalidXLogRecPtr;
	bufptr = buffer;
	while (!lastline)
	{
		char	   *ptr;
		char	   *token_end;
		char		save;
		TimeLineID	tli;
		XLogRecPtr	switchpoint;
		bool		valid_switchpoint = false;
		int			nchars;
		size_t		nspaces;
		char		uuid_str[UUID_STR_LEN + 1] = {0};

		fline = bufptr;
		while (*bufptr && *bufptr != '\n')
			bufptr++;
		if (!(*bufptr))
			lastline = true;
		else
			*bufptr++ = '\0';

		/* skip leading whitespace and check for # comment */
		for (ptr = fline; *ptr; ptr++)
		{
			if (!isspace((unsigned char) *ptr))
				break;
		}
		if (*ptr == '\0' || *ptr == '#')
			continue;

		if (sscanf(fline, "%u%n", &tli, &nchars) != 1)
		{
			/* expect a numeric timeline ID as first field of line */
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a numeric timeline ID.");
			exit(1);
		}

		/* the switchpoint location follows, separated by whitespace */
		ptr = fline + nchars;
		nspaces = strspn(ptr, " \t\n\r\f\v");
		if (nspaces > 0)
		{
			ptr += nspaces;

			/*
			 * isolate the location from the rest of the line before parsing
			 * it
			 */
			token_end = ptr + strcspn(ptr, " \t\n\r\f\v");
			save = *token_end;
			*token_end = '\0';
			valid_switchpoint = pg_parse_lsn(ptr, &switchpoint);
			*token_end = save;
			ptr = token_end;
		}

		if (!valid_switchpoint)
		{
			pg_log_error("syntax error in history file: %s", fline);
			pg_log_error_detail("Expected a write-ahead log switchpoint location.");
			exit(1);
		}
		if (entries && tli <= lasttli)
		{
			pg_log_error("invalid data in history file: %s", fline);
			pg_log_error_detail("Timeline IDs must be in increasing sequence.");
			exit(1);
		}

		lasttli = tli;

		nlines++;
		entries = pg_realloc_array(entries, TimeLineHistoryEntry, nlines);

		entry = &entries[nlines - 1];
		entry->tli = tli;
		entry->begin = prevend;
		entry->end = switchpoint;
		prevend = entry->end;

		/*
		 * Parse the optional UUID field that follows the switchpoint. Old
		 * history files have a human-readable reason string there instead;
		 * its first word is much shorter than UUID_STR_LEN, so the length
		 * check safely distinguishes old from new format.  We ignore the
		 * remainder of the line beyond the UUID field.
		 */
		memset(&entry->tluuid, 0, sizeof(pg_uuid_t));
		nspaces = strspn(ptr, " \t\n\r\f\v");
		if (nspaces > 0)
		{
			pg_uuid_t	buf;

			ptr += nspaces;
			token_end = ptr + strcspn(ptr, " \t\n\r\f\v");
			if ((size_t) (token_end - ptr) == UUID_STR_LEN)
			{
				memcpy(uuid_str, ptr, UUID_STR_LEN);
				uuid_str[UUID_STR_LEN] = '\0';
				if (rewind_parse_uuid(uuid_str, &buf))
					memcpy(&entry->tluuid, &buf, sizeof(pg_uuid_t));
			}
		}
	}

	if (entries && targetTLI <= lasttli)
	{
		pg_log_error("invalid data in history file");
		pg_log_error_detail("Timeline IDs must be less than child timeline's ID.");
		exit(1);
	}

	/*
	 * Create one more entry for the "tip" of the timeline, which has no entry
	 * in the history file.
	 */
	nlines++;
	if (entries)
		entries = pg_realloc_array(entries, TimeLineHistoryEntry, nlines);
	else
		entries = pg_malloc_array(TimeLineHistoryEntry, 1);

	entry = &entries[nlines - 1];
	entry->tli = targetTLI;
	entry->begin = prevend;
	entry->end = InvalidXLogRecPtr;
	memset(&entry->tluuid, 0, sizeof(pg_uuid_t));

	*nentries = nlines;
	return entries;
}
