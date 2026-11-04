/*-------------------------------------------------------------------------
 *
 * archive_module.h
 *		Exports for archive modules.
 *
 * Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 * src/include/archive/archive_module.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _ARCHIVE_MODULE_H
#define _ARCHIVE_MODULE_H

/*
 * The value of the archive_library GUC.
 */
extern PGDLLIMPORT char *XLogArchiveLibrary;

typedef struct ArchiveModuleState
{
	/*
	 * Private data pointer for use by an archive module.  This can be used to
	 * store state for the module that will be passed to each of its
	 * callbacks.
	 */
	void	   *private_data;
} ArchiveModuleState;

/*
 * Archive module callbacks
 *
 * Returned via _PG_archive_module_init().  Provide archive_file_cb, or
 * submit_cb plus poll_cb with cancel_cb.
 */
typedef enum ArchiveResultStatus
{
	ARCHIVE_RESULT_SUCCESS,
	ARCHIVE_RESULT_RETRY,
	ARCHIVE_RESULT_FAIL
} ArchiveResultStatus;

typedef void (*ArchiveStartupCB) (ArchiveModuleState *state);
typedef bool (*ArchiveCheckConfiguredCB) (ArchiveModuleState *state);
typedef bool (*ArchiveFileCB) (ArchiveModuleState *state, const char *file, const char *path);
typedef bool (*ArchiveSubmitCB) (ArchiveModuleState *state, const char *file, const char *path);
typedef bool (*ArchivePollCB) (ArchiveModuleState *state, const char **file,
							   ArchiveResultStatus *status, long timeout_ms);
typedef void (*ArchiveCancelCB) (ArchiveModuleState *state);
typedef void (*ArchiveShutdownCB) (ArchiveModuleState *state);

typedef struct ArchiveModuleCallbacks
{
	ArchiveStartupCB startup_cb;
	ArchiveCheckConfiguredCB check_configured_cb;
	ArchiveFileCB archive_file_cb;
	ArchiveShutdownCB shutdown_cb;
	ArchiveSubmitCB submit_cb;
	ArchivePollCB poll_cb;
	ArchiveCancelCB cancel_cb;
} ArchiveModuleCallbacks;

/*
 * Type of the shared library symbol _PG_archive_module_init that is looked
 * up when loading an archive library.
 */
typedef const ArchiveModuleCallbacks *(*ArchiveModuleInit) (void);

extern PGDLLEXPORT const ArchiveModuleCallbacks *_PG_archive_module_init(void);

/* Support for messages reported from archive module callbacks. */

extern PGDLLIMPORT char *arch_module_check_errdetail_string;

#define arch_module_check_errdetail \
	pre_format_elog_string(errno, TEXTDOMAIN), \
	arch_module_check_errdetail_string = format_elog_string

#endif							/* _ARCHIVE_MODULE_H */
