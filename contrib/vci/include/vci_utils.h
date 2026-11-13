/*-------------------------------------------------------------------------
 *
 * vci_utils.h
 *	  Debugging functions and macros
 *
 *
 * Portions Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/vci/include/vci_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VCI_DEBUG_H
#define VCI_DEBUG_H

#include "postgres.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nodes/nodes.h"

#include "vci.h"

/* obtain the node name of type */
extern PGDLLEXPORT const char *VciGetNodeName(NodeTag type);

/**
 * @brief Find value from unsorted array of int16 and returns the position.
 * @param[in] array Pointer to the array of int16.
 * @param[in] len Length of the array.
 * @param[in] value The value to find out.
 * @return The position of value.
 * When the value is not found, it returns -1.
 */
static inline int
FindInt16(int16 *array, int len, int16 value)
{
	int			ptr;

	for (ptr = 0; ptr < len; ++ptr)
		if (array[ptr] == value)
			return ptr;
	return -1;
}

/**
 * @brief Pfree and make the pointer null.
 *
 * @param[in, out] ptr When *ptr, NOT ptr ITSELF, is not NULL, pfree *ptr.
 * Then, put *ptr = NULL.
 * So use like * vci_PfreeAndNull(& pointer).
 */
static inline void
vci_PfreeAndNull(void *ptr)
{
	Assert(ptr);
	if (NULL == *(void **) ptr)
		return;
	pfree(*(void **) ptr);
	*(void **) ptr = NULL;
}

/**
 * @brief Allocate memory area with given size, and copy the given source
 * to the area newly allocated.
 *
 * @param[in] src Pointer to the array of byte data to be copied.
 * @param[in] size The size of source data pointed by src.
 */
static inline void *
vci_AllocateAndCopy(const void *src, Size size)
{
	if (src != NULL && size > 0)
	{
		void	   *dst = palloc(size);

		memcpy(dst, src, size);
		return dst;
	}
	return NULL;
}

/**
 * @brief A part of GetHighestBit().
 *
 * @note Do not use this function directly.
 */
static inline void
vci_GetHighestBitSub(int *result, uint64 *value, uint64 mask, int inc)
{
	if (mask & *value)
	{
		*result += inc;
		*value &= mask;
	}
	else
		*value &= ~mask;
}

/**
 * @brief Get the largest bit ID in bits set 1 in the given uint64 value.
 *
 * It should be 63 - CLZ(value) (Count Leading Zero).
 * If the given value is 0, returns -1.
 * Same as
 * \code{.c}
 * if (value & 0x8000000000000000) return 63;
 * if (value & 0x4000000000000000) return 62;
 * ...
 * if (value & 0x0000000000000002) return 1;
 * if (value & 0x0000000000000001) return 0;
 * return -1;
 * \endcode
 *
 * @param[in] value Value to examine.
 * @return The bit ID of MSB set, in a manner of zero-origin.
 * If no bit has 1, -1 is returned.
 *
 * @note Better to use count leading zero (CLZ), if possible.
 */
static inline int
vci_GetHighestBit(uint64 value)
{
	int			result = 0;

	if (0 == value)
		return -1;

	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xFFFFFFFF00000000), 32);
	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xFFFF0000FFFF0000), 16);
	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xFF00FF00FF00FF00), 8);
	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xF0F0F0F0F0F0F0F0), 4);
	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xCCCCCCCCCCCCCCCC), 2);
	vci_GetHighestBitSub(&result, &value, UINT64CONST(0xAAAAAAAAAAAAAAAA), 1);

	return result;
}

/**
 * @brief Calculate the ID of least significant bit (LSB) set, 1.
 *
 * Same as
 * \code{.c}
 * if (value & 0x8000000000000001) return 0;
 * if (value & 0x4000000000000002) return 1;
 * ...
 * if (value & 0x4000000000000000) return 62;
 * if (value & 0x8000000000000000) return 63;
 * return 63;
 * \endcode
 *
 * @param[in] value Value to examine.
 * @return The bit ID of LSB set, in a manner of zero-origin.
 * If no bit has 1, -1 is returned.
 *
 * @note Better to use count leading zero (CLZ) and reverse bit, if possible.
 */
static inline int
vci_GetLowestBit(uint64 value)
{
	int			result = 63;

	if (0 == value)
		return -1;

	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xFFFFFFFF00000000), -32);
	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xFFFF0000FFFF0000), -16);
	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xFF00FF00FF00FF00), -8);
	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xF0F0F0F0F0F0F0F0), -4);
	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xCCCCCCCCCCCCCCCC), -2);
	vci_GetHighestBitSub(&result, &value, ~UINT64CONST(0xAAAAAAAAAAAAAAAA), -1);
	return result;
}

/**
 * @brief Count number of bits set, 1.
 *
 * Same as
 * \code{.c}
 * uint64 count = 0;
 * if (value & 0x0000000000000001) ++ count;
 * if (value & 0x0000000000000002) ++ count;
 * ...
 * if (value & 0x4000000000000000) ++ count;
 * if (value & 0x8000000000000000) ++ count;
 * return count;
 * \endcode
 *
 * @param[in] value Value to examine.
 * @return The number of bit set.
 */
static inline int
vci_GetBitCount(uint64 value)
{
	uint64		count = 0;

	count = (value & UINT64CONST(0x5555555555555555)) + ((value >> 1) & UINT64CONST(0x5555555555555555));
	count = (count & UINT64CONST(0x3333333333333333)) + ((count >> 2) & UINT64CONST(0x3333333333333333));
	count = (count & UINT64CONST(0x0f0f0f0f0f0f0f0f)) + ((count >> 4) & UINT64CONST(0x0f0f0f0f0f0f0f0f));
	count = (count & UINT64CONST(0x00ff00ff00ff00ff)) + ((count >> 8) & UINT64CONST(0x00ff00ff00ff00ff));
	count = (count & UINT64CONST(0x0000ffff0000ffff)) + ((count >> 16) & UINT64CONST(0x0000ffff0000ffff));
	count = (count & UINT64CONST(0x00000000ffffffff)) + ((count >> 32) & UINT64CONST(0x00000000ffffffff));
	return (int) count;
}

/**
 * @brief Set specified bit in char array to 1.
 *
 * @param[in, out] bitData Pointer to an array of char.
 * @param bitID Bit ID to set.
 */
static inline void
vci_SetBit(char *bitData, uint16 bitId)
{
	bitData[bitId >> 3] |= 1 << (bitId & 7);
}

#endif							/* VCI_DEBUG_H */
