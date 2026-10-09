/*-------------------------------------------------------------------------
 *
 * uuid.h
 *	  Header file for the "uuid" ADT. In C, we use the name pg_uuid_t,
 *	  to avoid conflicts with any uuid_t type that might be defined by
 *	  the system headers.
 *
 * Copyright (c) 2007-2026, PostgreSQL Global Development Group
 *
 * src/include/utils/uuid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UUID_H
#define UUID_H

/* uuid size in bytes */
#define UUID_LEN 16

/* length of a UUID string (without null terminator): xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
#define UUID_STR_LEN 36

typedef struct pg_uuid_t
{
	unsigned char data[UUID_LEN];
} pg_uuid_t;

/* fmgr interface macros (backend only) */
#ifndef FRONTEND
static inline Datum
UUIDPGetDatum(const pg_uuid_t *X)
{
	return PointerGetDatum(X);
}

#define PG_RETURN_UUID_P(X)		return UUIDPGetDatum(X)

static inline pg_uuid_t *
DatumGetUUIDP(Datum X)
{
	return (pg_uuid_t *) DatumGetPointer(X);
}

#define PG_GETARG_UUID_P(X)		DatumGetUUIDP(PG_GETARG_DATUM(X))

extern pg_uuid_t *generate_uuidv7(uint64 unix_ts_ms, uint32 sub_ms);
extern pg_uuid_t *generate_uuidv7_r(pg_uuid_t *uuid, uint64 unix_ts_ms, uint32 sub_ms);

#endif							/* !FRONTEND */
#endif							/* UUID_H */
