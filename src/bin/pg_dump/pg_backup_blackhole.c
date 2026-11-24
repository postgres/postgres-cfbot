/*-------------------------------------------------------------------------
 *
 * pg_backup_blackhole.c
 *
 *	Implementation of an archive that is never saved and never outputs anything.
 *	It is used by pg_dump to execute COPY TO BLACKHOLE commands.
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_blackhole.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "parallel.h"
#include "pg_backup_archiver.h"
#include "pg_backup_utils.h"

static int	_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te);
static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);
static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);

/*
 *	Initializer
 */
void
InitArchiveFmt_Blackhole(ArchiveHandle *AH)
{
	/* Assuming static functions, this can be copied for each format. */
	AH->ArchiveEntryPtr = _ArchiveEntry;
	AH->StartDataPtr = _StartData;

	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = NULL;
	AH->PrintTocDataPtr = _PrintTocData;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;

	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	/* no parallel dump in the custom archive, only parallel restore */
	AH->WorkerJobDumpPtr = _WorkerJobDumpDirectory;

	if (AH->mode == archModeRead)
		pg_fatal("this format cannot be read");
}

static int
_WorkerJobDumpDirectory(ArchiveHandle *AH, TocEntry *te)
{
	WriteDataChunksForTocEntry(AH, te);

	/* Return nothing */
	return 0;
}

/*
 * Those must be non-NULL, because CloneArchive() / DeCloneArchive() invokes
 * them.
 */
static void
_Clone(ArchiveHandle *AH)
{
	/* Do nothing */
}

static void
_DeClone(ArchiveHandle *AH)
{
	/* Do nothing */
}

static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	/* Do nothing */
}

static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	/* Do nothing */
}

static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	/* Do nothing */
}

static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	/* Do nothing */
}

static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	/* Do nothing */
}

static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	/* Do nothing */
}

static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	/* Do nothing */
}

static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	/* Do nothing */
}

static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	if (te->dataDumper)
	{
		AH->currToc = te;
		te->dataDumper((Archive *) AH, te->dataDumperArg);
		AH->currToc = NULL;
	}
}

static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	return 0;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	/* Do nothing */
}

/*
 * Close the archive.
 *
 * When writing the archive, this is the routine that actually starts
 * the process of saving it to files.
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	ParallelState *pstate;

	if (AH->mode != archModeWrite)
		return;

	/*
	 * WriteDataChunks() calls TocEntry's dataDumper (dumpTableData_copy) that
	 * issues COPY table TO BLACKHOLE.
	 */
	pstate = ParallelBackupStart(AH);
	WriteDataChunks(AH, pstate);
	ParallelBackupEnd(AH, pstate);
}
