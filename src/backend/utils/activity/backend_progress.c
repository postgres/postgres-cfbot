/* ----------
 * backend_progress.c
 *
 *	Command progress reporting infrastructure.
 *
 *	Copyright (c) 2001-2026, PostgreSQL Global Development Group
 *
 *	src/backend/utils/activity/backend_progress.c
 * ----------
 */
#include "postgres.h"

#include "access/parallel.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/backend_progress.h"
#include "utils/backend_status.h"


#ifdef PROGRESS_DEBUG

/*
 * Room for every parameter as " index:old->new".  The log line is built on
 * the stack: progress can be reported inside a critical section, where
 * palloc is not allowed.
 */
#define PROGRESS_DEBUG_BUFSIZE	(PGSTAT_NUM_PROGRESS_PARAM * 48)

static const char *
progress_debug_command_name(ProgressCommandType cmdtype)
{
	switch (cmdtype)
	{
		case PROGRESS_COMMAND_INVALID:
			return "INVALID";
		case PROGRESS_COMMAND_VACUUM:
			return "VACUUM";
		case PROGRESS_COMMAND_ANALYZE:
			return "ANALYZE";
		case PROGRESS_COMMAND_CREATE_INDEX:
			return "CREATE_INDEX";
		case PROGRESS_COMMAND_BASEBACKUP:
			return "BASEBACKUP";
		case PROGRESS_COMMAND_COPY:
			return "COPY";
		case PROGRESS_COMMAND_REPACK:
			return "REPACK";
		case PROGRESS_COMMAND_DATACHECKSUMS:
			return "DATACHECKSUMS";
	}
	return "UNKNOWN";
}

/*
 * Log one change of this backend's progress state.
 *
 * The format is meant to be parsed by tests (see src/test/modules/
 * test_progress), so keep it stable:
 *
 *   progress start: <command> relid=<oid>
 *   progress update: <command> relid=<oid> <index>:<old>-><new> ...
 *   progress end: <command> relid=<oid>
 *
 * An update line lists only the parameters whose value changed, and one
 * pgstat_progress_update_multi_param() call produces one line, since
 * readers see those values change together.
 *
 * This must be called after PGSTAT_END_WRITE_ACTIVITY(), outside the
 * critical section that protects the write.
 *
 * Standalone backends, such as those initdb runs, log to their caller's
 * stderr, so nothing is logged there: that output should not change with
 * this option.
 */
static void
progress_debug_log(const char *event, ProgressCommandType cmdtype, Oid relid,
				   const char *changes)
{
	if (!IsUnderPostmaster)
		return;

	/* LOG_SERVER_ONLY: never sent to the client, so no test output changes */
	ereport(LOG_SERVER_ONLY,
			errmsg_internal("progress %s: %s relid=%u%s",
							event,
							progress_debug_command_name(cmdtype),
							relid,
							changes ? changes : ""),
			errhidestmt(true),
			errhidecontext(true));
}

static int
progress_debug_append(char *buf, int len, int index, int64 oldval, int64 newval)
{
	int			n;

	n = snprintf(buf + len, PROGRESS_DEBUG_BUFSIZE - len,
				 " %d:%lld->%lld", index,
				 (long long) oldval, (long long) newval);
	Assert(n > 0 && len + n < PROGRESS_DEBUG_BUFSIZE);
	return len + n;
}

#endif							/* PROGRESS_DEBUG */

/*-----------
 * pgstat_progress_start_command() -
 *
 * Set st_progress_command (and st_progress_command_target) in own backend
 * entry.  Also, zero-initialize st_progress_param array.
 *-----------
 */
void
pgstat_progress_start_command(ProgressCommandType cmdtype, Oid relid)
{
	volatile PgBackendStatus *beentry = MyBEEntry;

	if (!beentry || !pgstat_track_activities)
		return;

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_command = cmdtype;
	beentry->st_progress_command_target = relid;
	MemSet(&beentry->st_progress_param, 0, sizeof(beentry->st_progress_param));
	PGSTAT_END_WRITE_ACTIVITY(beentry);

#ifdef PROGRESS_DEBUG
	progress_debug_log("start", cmdtype, relid, NULL);
#endif
}

/*-----------
 * pgstat_progress_update_param() -
 *
 * Update index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_update_param(int index, int64 val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
#ifdef PROGRESS_DEBUG
	int64		oldval;
#endif

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || !pgstat_track_activities)
		return;

#ifdef PROGRESS_DEBUG
	oldval = beentry->st_progress_param[index];
#endif

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_param[index] = val;
	PGSTAT_END_WRITE_ACTIVITY(beentry);

#ifdef PROGRESS_DEBUG
	if (oldval != val)
	{
		char		changes[PROGRESS_DEBUG_BUFSIZE];

		progress_debug_append(changes, 0, index, oldval, val);
		progress_debug_log("update", beentry->st_progress_command,
						   beentry->st_progress_command_target, changes);
	}
#endif
}

/*-----------
 * pgstat_progress_incr_param() -
 *
 * Increment index'th member in st_progress_param[] of own backend entry.
 *-----------
 */
void
pgstat_progress_incr_param(int index, int64 incr)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
#ifdef PROGRESS_DEBUG
	int64		oldval;
#endif

	Assert(index >= 0 && index < PGSTAT_NUM_PROGRESS_PARAM);

	if (!beentry || !pgstat_track_activities)
		return;

#ifdef PROGRESS_DEBUG
	oldval = beentry->st_progress_param[index];
#endif

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_param[index] += incr;
	PGSTAT_END_WRITE_ACTIVITY(beentry);

#ifdef PROGRESS_DEBUG
	if (incr != 0)
	{
		char		changes[PROGRESS_DEBUG_BUFSIZE];

		progress_debug_append(changes, 0, index, oldval, oldval + incr);
		progress_debug_log("update", beentry->st_progress_command,
						   beentry->st_progress_command_target, changes);
	}
#endif
}

/*-----------
 * pgstat_progress_parallel_incr_param() -
 *
 * A variant of pgstat_progress_incr_param to allow a worker to poke at
 * a leader to do an incremental progress update.
 *-----------
 */
void
pgstat_progress_parallel_incr_param(int index, int64 incr)
{
	/*
	 * Parallel workers notify a leader through a PqParallelMsg_Progress
	 * message to update progress, passing the progress index and incremented
	 * value. Leaders can just call pgstat_progress_incr_param directly.
	 */
	if (IsParallelWorker())
	{
		static StringInfoData progress_message;

		pq_beginmessage(&progress_message, PqParallelMsg_Progress);
		pq_sendint32(&progress_message, index);
		pq_sendint64(&progress_message, incr);
		pq_endmessage(&progress_message);
	}
	else
		pgstat_progress_incr_param(index, incr);
}

/*-----------
 * pgstat_progress_update_multi_param() -
 *
 * Update multiple members in st_progress_param[] of own backend entry.
 * This is atomic; readers won't see intermediate states.
 *-----------
 */
void
pgstat_progress_update_multi_param(int nparam, const int *index,
								   const int64 *val)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
	int			i;
#ifdef PROGRESS_DEBUG
	int64		oldval[PGSTAT_NUM_PROGRESS_PARAM];
#endif

	if (!beentry || !pgstat_track_activities || nparam == 0)
		return;

#ifdef PROGRESS_DEBUG
	for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; ++i)
		oldval[i] = beentry->st_progress_param[i];
#endif

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);

	for (i = 0; i < nparam; ++i)
	{
		Assert(index[i] >= 0 && index[i] < PGSTAT_NUM_PROGRESS_PARAM);

		beentry->st_progress_param[index[i]] = val[i];
	}

	PGSTAT_END_WRITE_ACTIVITY(beentry);

#ifdef PROGRESS_DEBUG
	{
		char		changes[PROGRESS_DEBUG_BUFSIZE];
		int			len = 0;

		/*
		 * Report each changed parameter once, with its final value, in index
		 * order.  An index could appear more than once in the call.
		 */
		for (i = 0; i < PGSTAT_NUM_PROGRESS_PARAM; ++i)
		{
			if (beentry->st_progress_param[i] != oldval[i])
				len = progress_debug_append(changes, len, i, oldval[i],
											beentry->st_progress_param[i]);
		}
		if (len > 0)
			progress_debug_log("update", beentry->st_progress_command,
							   beentry->st_progress_command_target, changes);
	}
#endif
}

/*-----------
 * pgstat_progress_end_command() -
 *
 * Reset st_progress_command (and st_progress_command_target) in own backend
 * entry.  This signals the end of the command.
 *-----------
 */
void
pgstat_progress_end_command(void)
{
	volatile PgBackendStatus *beentry = MyBEEntry;
#ifdef PROGRESS_DEBUG
	ProgressCommandType cmdtype;
	Oid			relid;
#endif

	if (!beentry || !pgstat_track_activities)
		return;

	if (beentry->st_progress_command == PROGRESS_COMMAND_INVALID)
		return;

#ifdef PROGRESS_DEBUG
	cmdtype = beentry->st_progress_command;
	relid = beentry->st_progress_command_target;
#endif

	PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
	beentry->st_progress_command = PROGRESS_COMMAND_INVALID;
	beentry->st_progress_command_target = InvalidOid;
	PGSTAT_END_WRITE_ACTIVITY(beentry);

#ifdef PROGRESS_DEBUG
	progress_debug_log("end", cmdtype, relid, NULL);
#endif
}
