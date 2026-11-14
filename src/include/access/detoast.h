/*-------------------------------------------------------------------------
 *
 * detoast.h
 *	  Access to compressed and external varlena values.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * src/include/access/detoast.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DETOAST_H
#define DETOAST_H

#include "access/toast_compression.h"
#include "varatt.h"

/*
 * Macro to fetch the possibly-unaligned contents of an EXTERNAL datum
 * into a local "varatt_external_oid" toast pointer.  This should be
 * just a memcpy, but some versions of gcc seem to produce broken code
 * that assumes the datum contents are aligned.  Introducing an explicit
 * intermediate "varattrib_1b_e *" variable seems to fix it.
 */
#define VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr) \
do { \
	varattrib_1b_e *attre = (varattrib_1b_e *) (attr); \
	Assert(VARATT_IS_EXTERNAL(attre)); \
	Assert(VARSIZE_EXTERNAL(attre) == sizeof(toast_pointer) + VARHDRSZ_EXTERNAL); \
	memcpy(&(toast_pointer), VARDATA_EXTERNAL(attre), sizeof(toast_pointer)); \
} while (0)

/* Size of an EXTERNAL datum that contains a standard TOAST pointer */
#define TOAST_OID_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external_oid))

/* Size of an EXTERNAL datum that contains an Oid8 TOAST pointer */
#define TOAST_OID8_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_external_oid8))

/* Size of an EXTERNAL datum that contains an indirection pointer */
#define INDIRECT_POINTER_SIZE (VARHDRSZ_EXTERNAL + sizeof(varatt_indirect))

/*
 * Decoded contents of an on-disk external TOAST pointer.
 *
 * Callers use these fields rather than decoding va_extinfo themselves.
 * compress_method is the actual method ID whichever pointer form was used.
 * It is TOAST_INVALID_COMPRESSION_ID when the value is not compressed, and
 * also when is_compressed is set but the pointer is inconsistent (see
 * toast_external_info_get()); method_bits and method_byte keep what is
 * stored so that callers such as amcheck can tell which.
 */
typedef struct toast_external_data
{
	vartag_external tag;		/* VARTAG_ONDISK_* */
	int32		rawsize;		/* original data size (includes header) */
	int32		extsize;		/* external saved size (without header) */
	bool		is_compressed;	/* is the saved data compressed? */
	ToastCompressionId compress_method; /* compression method ID */
	uint8		method_bits;	/* raw method bits of va_extinfo */
	int16		method_byte;	/* trailing method byte, or -1 if plain form */
	Oid8		valueid;		/* value ID (can be widened from Oid) */
	Oid			toastrelid;		/* OID of the TOAST table containing it */
} toast_external_data;

/*
 * Decode an on-disk external TOAST pointer into a toast_external_data.
 */
static inline void
toast_external_info_get(const struct varlena *attr, toast_external_data *toast_ext_data)
{
	const char *ptr;
	Size		fixedsize;
	uint32		extinfo;

	Assert(VARATT_IS_EXTERNAL_ONDISK(attr));

	/*
	 * The long form of a pointer is the plain form followed by the
	 * compression method ID byte, so decoding the fixed part is the same for
	 * both.  We can't use VARATT_EXTERNAL_GET_POINTER() here because it
	 * insists on the datum being exactly the size of the struct.
	 */
	ptr = VARDATA_EXTERNAL(attr);
	toast_ext_data->tag = VARTAG_EXTERNAL(attr);
	if (toast_ext_data->tag == VARTAG_ONDISK_OID8 ||
		toast_ext_data->tag == VARTAG_ONDISK_OID8_LONG)
	{
		varatt_external_oid8 toast_pointer;

		memcpy(&toast_pointer, ptr, sizeof(toast_pointer));
		toast_ext_data->rawsize = toast_pointer.va_rawsize;
		extinfo = toast_pointer.va_extinfo;
		toast_ext_data->valueid = VARATT_EXTERNAL_OID8_GET_VALUEID(toast_pointer);
		toast_ext_data->toastrelid = toast_pointer.va_toastrelid;
		fixedsize = sizeof(toast_pointer);
	}
	else
	{
		varatt_external_oid toast_pointer;

		memcpy(&toast_pointer, ptr, sizeof(toast_pointer));
		toast_ext_data->rawsize = toast_pointer.va_rawsize;
		extinfo = toast_pointer.va_extinfo;
		toast_ext_data->valueid = toast_pointer.va_valueid;
		toast_ext_data->toastrelid = toast_pointer.va_toastrelid;
		fixedsize = sizeof(toast_pointer);
	}

	toast_ext_data->extsize = VARATT_EXTINFO_GET_EXTSIZE(extinfo);
	toast_ext_data->is_compressed =
		VARATT_EXTINFO_IS_COMPRESSED(extinfo, toast_ext_data->rawsize);
	toast_ext_data->method_bits = extinfo >> VARLENA_EXTSIZE_BITS;
	toast_ext_data->method_byte = -1;
	if (VARTAG_IS_ONDISK_LONG(toast_ext_data->tag))
	{
		uint8		cmid;

		memcpy(&cmid, ptr + fixedsize, sizeof(cmid));
		toast_ext_data->method_byte = cmid;
	}

	/*
	 * Resolve the compression method: it is the two method bits of extinfo,
	 * except in the long form of a pointer, where those bits only flag the
	 * form and the method is in the trailing byte.
	 *
	 * A long-form pointer whose method bits say otherwise is corrupt, and so
	 * is one whose trailing byte names a method that uses the plain form or
	 * no method at all.  Likewise, a plain pointer can only name pglz or lz4.
	 * This is also used on data read from disk, notably by amcheck, so don't
	 * assert on that: report an invalid method and let the caller deal with
	 * it.  An external datum that is not compressed has no method either.
	 */
	toast_ext_data->compress_method =
		toast_compression_id_resolve(toast_ext_data->method_bits,
									 toast_ext_data->method_byte);
	if (!toast_ext_data->is_compressed)
		toast_ext_data->compress_method = TOAST_INVALID_COMPRESSION_ID;
}

/* ----------
 * detoast_external_attr() -
 *
 *		Fetches an external stored attribute from the toast
 *		relation. Does NOT decompress it, if stored external
 *		in compressed format.
 * ----------
 */
extern varlena *detoast_external_attr(varlena *attr);

/* ----------
 * detoast_attr() -
 *
 *		Fully detoasts one attribute, fetching and/or decompressing
 *		it as needed.
 * ----------
 */
extern varlena *detoast_attr(varlena *attr);

/* ----------
 * detoast_attr_slice() -
 *
 *		Fetches only the specified portion of an attribute.
 *		(Handles all cases for attribute storage)
 * ----------
 */
extern varlena *detoast_attr_slice(varlena *attr,
								   int32 sliceoffset,
								   int32 slicelength);

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 */
extern Size toast_raw_datum_size(Datum value);

/* ----------
 * toast_datum_size -
 *
 *	Return the storage size of a varlena datum
 * ----------
 */
extern Size toast_datum_size(Datum value);

#endif							/* DETOAST_H */
