/*-------------------------------------------------------------------------
 *
 * pg_job_object.h
 *	  Windows Job Object support for preventing orphaned backends
 *
 * When the postmaster crashes on Windows, child processes continue running
 * because Windows has no equivalent to Unix's parent death detection. Job
 * Objects solve this by allowing the kernel to terminate all children when
 * the job handle closes (which happens automatically on process exit).
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/pg_job_object.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_JOB_OBJECT_H
#define PG_JOB_OBJECT_H

#ifdef WIN32

extern void pg_create_job_object(void);
extern void pg_destroy_job_object(void);
extern bool pg_is_in_job_object(void);

#else							/* !WIN32 */

#define pg_create_job_object() ((void) 0)
#define pg_destroy_job_object() ((void) 0)
#define pg_is_in_job_object() (false)

#endif							/* WIN32 */

#endif							/* PG_JOB_OBJECT_H */
