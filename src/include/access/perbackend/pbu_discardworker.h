/*-------------------------------------------------------------------------
 *
 * pbu_discardworker.h
 *	  Exports from access/undo/perbackend/pbu_discardworker.c.
 *
 * Per-backend UNDO engine vendored from EnterpriseDB zheap
 * (Amit Kapila, Dilip Kumar, Kuntal Ghosh, Mahendra Singh Thalor,
 *  Rafia Sabih, Thomas Munro et al.).
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/perbackend/pbu_discardworker.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PBU_DISCARDWORKER_H
#define PBU_DISCARDWORKER_H

extern void DiscardWorkerRegister(void);
pg_noreturn extern void DiscardWorkerMain(Datum main_arg);
extern bool IsDiscardProcess(void);

#endif							/* PBU_DISCARDWORKER_H */
