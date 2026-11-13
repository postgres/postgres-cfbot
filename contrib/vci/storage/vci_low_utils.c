/*-------------------------------------------------------------------------
 *
 * vci_low_utils.c
 *	  Low-level utility function
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/storage/vci_low_utils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/snapmgr.h"

#include "vci.h"
#include "vci_ros.h"

/**
 * @brief This function writes data over pages.
 *
 * The last page is not flushed.
 * So, after calling for the last data, the last page in the return value
 * must be written by functions like vci_WriteOnePageIfNecessaryAndGetBuffer().
 *
 * @param[in] rel relation to store the data.
 * @param[in, out] blockNumber the first block number to write as input.
 * the blockNumber for the next data.
 * @param[in, out] blockNumberOld the block number of buffer in the argument.
 * the blockNumber of the buffer returned as output.
 * @param[in, out] offsetInPage the offset in the page to write as input.
 * the offset in the page of the next data as output.
 * @param[in] buffer shared buffer of *blockNumber.
 * @param[in] data_ the pointer of the data to write.
 * @param[in] size the size of the data to write.
 * @return the shared buffer read last, which is not written.
 */
Buffer
vci_WriteDataIntoMultiplePages(Relation rel,
							   BlockNumber *blockNumber,
							   BlockNumber *blockNumberOld,
							   uint32 *offsetInPage,
							   Buffer buffer,
							   const void *data_,
							   Size size)
{
	const char *data = (const char *) data_;

	Assert(*offsetInPage < VCI_MAX_PAGE_SPACE);
	for (Size ptr = 0; ptr < size;)
	{
		Page		page;
		uint32		writeSize;

		writeSize = Min(VCI_MAX_PAGE_SPACE - *offsetInPage, size - ptr);
		buffer = vci_WriteOnePageIfNecessaryAndGetBuffer(rel,
														 *blockNumber,
														 *blockNumberOld,
														 buffer);
		*blockNumberOld = *blockNumber;
		page = BufferGetPage(buffer);
		memcpy(&(page[VCI_MIN_PAGE_HEADER + *offsetInPage]), &(data[ptr]),
			   writeSize);
		ptr += writeSize;
		*offsetInPage += writeSize;
		if (VCI_MAX_PAGE_SPACE <= *offsetInPage)
		{
			++(*blockNumber);
			*offsetInPage = 0;
		}
	}

	return buffer;
}

/**
 * @brief Get active snapshot and push it, update command ID.
 *
 * @return active snapshot.
 */
Snapshot
vci_GetCurrentSnapshot(void)
{
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	return GetActiveSnapshot();
}
